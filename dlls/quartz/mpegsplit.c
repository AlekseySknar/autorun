/*
 * MPEG audio splitter
 *
 * Copyright 2026 Wine-NX contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/* Reads MPEG audio files (.mp3, .mp2) without GStreamer, for systems where
 * winegstreamer, which normally provides quartz's MPEG-I splitter, is not
 * available. The one output pin carries the file's MPEG audio frames, one
 * frame to a sample and timed from the frame headers, for the ACM wrapper to
 * decode through l3codeca.acm. MPEG system and video streams are refused. */

#include <stdbool.h>
#include <stddef.h>

#include "quartz_private.h"
#include "mmreg.h"

WINE_DEFAULT_DEBUG_CHANNEL(quartz);

/* FOURCC media subtypes: the format tag in Data1. */
static const GUID fourcc_subtype = {0x00000000, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

/* Larger files are not MPEG audio a program would play through DirectShow. */
#define MPEG_MAX_FILE_SIZE (256 * 1024 * 1024)
/* How far into the file, after any ID3v2 tag, the first frame may start. */
#define MPEG_MAX_JUNK (64 * 1024)
/* Frames that must follow each other for the start to count as audio. */
#define MPEG_SYNC_FRAMES 3

struct mpeg_header
{
    unsigned int version;  /* 1 for MPEG-1, 2 for MPEG-2, 3 for MPEG-2.5 */
    unsigned int layer;    /* 1, 2 or 3 */
    unsigned int bitrate;  /* bits per second */
    unsigned int rate;     /* samples per second */
    unsigned int channels, mode, mode_ext, emphasis;
    unsigned int size;     /* of the whole frame, header included */
    unsigned int samples;  /* per channel in the frame */
    bool crc, copyright, original, private_bit;
};

struct mpeg_frame
{
    DWORD offset, size;
    REFERENCE_TIME start, end;
};

struct mpeg_splitter
{
    struct strmbase_filter filter;
    struct strmbase_sink sink;
    IAsyncReader *reader;

    /* The output pin and what it delivers; there only while the sink is connected. */
    bool has_source;
    struct strmbase_source source;
    SourceSeeking seek;
    AM_MEDIA_TYPE mt;

    BYTE *data;  /* the whole file */
    struct mpeg_frame *frames;
    size_t frame_count, frame_capacity;
    DWORD max_frame_size;

    /* Held by the streaming thread while it delivers a frame, and by
     * application threads to reposition or stop it. */
    CRITICAL_SECTION flushing_cs;
    CONDITION_VARIABLE eos_cv;
    HANDLE thread;

    /* Used by the streaming thread, and by application threads only while that
     * thread is not running or is kept out by flushing_cs. */
    size_t next;
    bool streaming, need_segment, discontinuity, eos;
};

static inline struct mpeg_splitter *impl_from_strmbase_filter(struct strmbase_filter *iface)
{
    return CONTAINING_RECORD(iface, struct mpeg_splitter, filter);
}

static inline struct mpeg_splitter *impl_from_strmbase_sink(struct strmbase_sink *iface)
{
    return CONTAINING_RECORD(iface, struct mpeg_splitter, sink);
}

static inline struct mpeg_splitter *impl_from_strmbase_pin(struct strmbase_pin *iface)
{
    return CONTAINING_RECORD(iface, struct mpeg_splitter, source.pin);
}

static inline struct mpeg_splitter *impl_from_IMediaSeeking(IMediaSeeking *iface)
{
    return CONTAINING_RECORD(iface, struct mpeg_splitter, seek.IMediaSeeking_iface);
}

/* Kilobits per second, by version (MPEG-1, or MPEG-2 and 2.5), layer and index. */
static const unsigned short bitrates[2][3][15] =
{
    {
        {0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448},
        {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384},
        {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320},
    },
    {
        {0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256},
        {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160},
        {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160},
    },
};

static const unsigned int sample_rates[3][3] =
{
    {44100, 48000, 32000},
    {22050, 24000, 16000},
    {11025, 12000, 8000},
};

/* A frame header; free-format streams, with no bitrate in the header, are not read. */
static bool parse_header(const BYTE *p, struct mpeg_header *h)
{
    unsigned int version_bits = (p[1] >> 3) & 3, layer_bits = (p[1] >> 1) & 3;
    unsigned int bitrate_index = p[2] >> 4, rate_index = (p[2] >> 2) & 3, padding = (p[2] >> 1) & 1;

    if (p[0] != 0xff || (p[1] & 0xe0) != 0xe0)
        return false;
    if (version_bits == 1 || !layer_bits || !bitrate_index || bitrate_index == 15
            || rate_index == 3 || (p[3] & 3) == 2)
        return false;

    h->version = version_bits == 3 ? 1 : version_bits == 2 ? 2 : 3;
    h->layer = 4 - layer_bits;
    h->bitrate = bitrates[h->version != 1][h->layer - 1][bitrate_index] * 1000;
    h->rate = sample_rates[h->version - 1][rate_index];
    h->crc = !(p[1] & 1);
    h->private_bit = p[2] & 1;
    h->mode = p[3] >> 6;
    h->mode_ext = (p[3] >> 4) & 3;
    h->copyright = !!(p[3] & 8);
    h->original = !!(p[3] & 4);
    h->emphasis = p[3] & 3;
    h->channels = h->mode == 3 ? 1 : 2;

    if (h->layer == 1)
    {
        h->samples = 384;
        h->size = (12 * h->bitrate / h->rate + padding) * 4;
    }
    else if (h->layer == 2 || h->version == 1)
    {
        h->samples = 1152;
        h->size = 144 * h->bitrate / h->rate + padding;
    }
    else
    {
        h->samples = 576;
        h->size = 72 * h->bitrate / h->rate + padding;
    }
    return h->size > 4;
}

/* Whether frames of the same kind follow each other from pos, as in audio
 * and not in a sync word that happens to be in other data. */
static bool frames_follow(const BYTE *data, size_t size, size_t pos, const struct mpeg_header *first)
{
    struct mpeg_header h;
    unsigned int i;

    for (i = 0; i < MPEG_SYNC_FRAMES; ++i)
    {
        if (size - pos < 4 || !parse_header(data + pos, &h))
            return i && size - pos < 4;  /* a short file ends after its frames */
        if (h.version != first->version || h.layer != first->layer || h.rate != first->rate)
            return false;
        if (h.size > size - pos)
            return false;
        pos += h.size;
    }
    return true;
}

/* An ID3v2 tag at the start: "ID3", a version, flags and a syncsafe size. */
static size_t skip_id3v2(const BYTE *data, size_t size)
{
    size_t pos = 0;

    while (size - pos >= 10 && !memcmp(data + pos, "ID3", 3) && data[pos + 3] != 0xff && data[pos + 4] != 0xff
            && !((data[pos + 6] | data[pos + 7] | data[pos + 8] | data[pos + 9]) & 0x80))
    {
        size_t len = ((size_t)data[pos + 6] << 21) | (data[pos + 7] << 14) | (data[pos + 8] << 7) | data[pos + 9];

        len += 10;
        if (data[pos + 5] & 0x10)
            len += 10;  /* a footer */
        if (len > size - pos)
            return size;
        pos += len;
    }
    return pos;
}

static bool add_frame(struct mpeg_splitter *filter, size_t offset, DWORD size, REFERENCE_TIME start, REFERENCE_TIME end)
{
    struct mpeg_frame *frame;

    if (!array_reserve((void **)&filter->frames, &filter->frame_capacity,
            filter->frame_count + 1, sizeof(*filter->frames)))
        return false;
    frame = &filter->frames[filter->frame_count++];
    frame->offset = offset;
    frame->size = size;
    frame->start = start;
    frame->end = end;
    filter->max_frame_size = max(filter->max_frame_size, size);
    return true;
}

/* Every frame from the first to the end of the file or a tag after the audio
 * (ID3v1, APE, Lyrics); a damaged stretch is skipped to the next good frame. */
static HRESULT index_frames(struct mpeg_splitter *filter, size_t size, size_t pos, const struct mpeg_header *first)
{
    const BYTE *data = filter->data;
    ULONGLONG samples = 0;
    struct mpeg_header h;

    while (size - pos >= 4)
    {
        if (!parse_header(data + pos, &h) || h.version != first->version || h.layer != first->layer
                || h.rate != first->rate || h.size > size - pos)
        {
            ++pos;
            continue;
        }
        if (!add_frame(filter, pos, h.size, (REFERENCE_TIME)(samples * 10000000 / h.rate),
                (REFERENCE_TIME)((samples + h.samples) * 10000000 / h.rate)))
            return E_OUTOFMEMORY;
        samples += h.samples;
        pos += h.size;
    }
    return filter->frame_count ? S_OK : VFW_E_INVALID_FILE_FORMAT;
}

/* WAVE_FORMAT_MPEGLAYER3 for layer III, which l3codeca.acm wants with its
 * extra fields; WAVE_FORMAT_MPEG with MPEG1WAVEFORMAT for layers I and II. */
static HRESULT init_media_type(struct mpeg_splitter *filter, const struct mpeg_header *h)
{
    WAVEFORMATEX *wfx;
    DWORD size;

    size = h->layer == 3 ? sizeof(MPEGLAYER3WAVEFORMAT) : sizeof(MPEG1WAVEFORMAT);
    if (!(wfx = CoTaskMemAlloc(size)))
        return E_OUTOFMEMORY;
    memset(wfx, 0, size);
    wfx->nChannels = h->channels;
    wfx->nSamplesPerSec = h->rate;
    wfx->nAvgBytesPerSec = h->bitrate / 8;
    wfx->cbSize = size - sizeof(WAVEFORMATEX);

    if (h->layer == 3)
    {
        MPEGLAYER3WAVEFORMAT *mp3 = (MPEGLAYER3WAVEFORMAT *)wfx;

        wfx->wFormatTag = WAVE_FORMAT_MPEGLAYER3;
        wfx->nBlockAlign = 1;
        mp3->wID = MPEGLAYER3_ID_MPEG;
        mp3->fdwFlags = MPEGLAYER3_FLAG_PADDING_ISO;
        mp3->nBlockSize = h->size;
        mp3->nFramesPerBlock = 1;
        mp3->nCodecDelay = 1393;
    }
    else
    {
        static const WORD modes[] = {ACM_MPEG_STEREO, ACM_MPEG_JOINTSTEREO, ACM_MPEG_DUALCHANNEL, ACM_MPEG_SINGLECHANNEL};
        MPEG1WAVEFORMAT *mpeg = (MPEG1WAVEFORMAT *)wfx;

        wfx->wFormatTag = WAVE_FORMAT_MPEG;
        wfx->nBlockAlign = h->size;
        mpeg->fwHeadLayer = h->layer == 1 ? ACM_MPEG_LAYER1 : ACM_MPEG_LAYER2;
        mpeg->dwHeadBitrate = h->bitrate;
        mpeg->fwHeadMode = modes[h->mode];
        mpeg->fwHeadModeExt = 1 << h->mode_ext;
        mpeg->wHeadEmphasis = h->emphasis + 1;
        mpeg->fwHeadFlags = (h->private_bit ? ACM_MPEG_PRIVATEBIT : 0) | (h->copyright ? ACM_MPEG_COPYRIGHT : 0)
                | (h->original ? ACM_MPEG_ORIGINALHOME : 0) | (h->crc ? ACM_MPEG_PROTECTIONBIT : 0)
                | (h->version == 1 ? ACM_MPEG_ID_MPEG1 : 0);
    }

    memset(&filter->mt, 0, sizeof(filter->mt));
    filter->mt.majortype = MEDIATYPE_Audio;
    filter->mt.subtype = fourcc_subtype;
    filter->mt.subtype.Data1 = wfx->wFormatTag;
    filter->mt.bFixedSizeSamples = TRUE;
    filter->mt.lSampleSize = 1;
    filter->mt.formattype = FORMAT_WaveFormatEx;
    filter->mt.cbFormat = size;
    filter->mt.pbFormat = (BYTE *)wfx;
    return S_OK;
}

static const struct strmbase_source_ops source_ops;
static const IMediaSeekingVtbl seeking_vtbl;

static HRESULT WINAPI mpeg_change_current(IMediaSeeking *iface)
{
    return S_OK;
}

static HRESULT WINAPI mpeg_change_stop(IMediaSeeking *iface)
{
    return S_OK;
}

static HRESULT WINAPI mpeg_change_rate(IMediaSeeking *iface)
{
    return S_OK;
}

static void remove_source(struct mpeg_splitter *filter)
{
    if (filter->has_source)
    {
        if (filter->source.pin.peer)
        {
            if (SUCCEEDED(IMemAllocator_Decommit(filter->source.pAllocator)))
                IPin_Disconnect(filter->source.pin.peer);
            IPin_Disconnect(&filter->source.pin.IPin_iface);
        }
        strmbase_seeking_cleanup(&filter->seek);
        strmbase_source_cleanup(&filter->source);
        FreeMediaType(&filter->mt);
        filter->has_source = false;
        BaseFilterImpl_IncrementPinVersion(&filter->filter);
    }
    free(filter->frames);
    filter->frames = NULL;
    filter->frame_count = filter->frame_capacity = 0;
    filter->max_frame_size = 0;
    free(filter->data);
    filter->data = NULL;
}

static HRESULT mpeg_sink_connect(struct strmbase_sink *iface, IPin *peer, const AM_MEDIA_TYPE *mt)
{
    struct mpeg_splitter *filter = impl_from_strmbase_sink(iface);
    LONGLONG file_size, available;
    struct mpeg_header first;
    size_t pos, start, size;
    bool found = false;
    HRESULT hr;

    if (FAILED(hr = IPin_QueryInterface(peer, &IID_IAsyncReader, (void **)&filter->reader)))
        return hr;
    IAsyncReader_Length(filter->reader, &file_size, &available);
    if (file_size < 4 || file_size > MPEG_MAX_FILE_SIZE)
    {
        hr = VFW_E_TYPE_NOT_ACCEPTED;
        goto fail;
    }
    size = file_size;
    if (!(filter->data = malloc(size)))
    {
        hr = E_OUTOFMEMORY;
        goto fail;
    }
    if ((hr = IAsyncReader_SyncRead(filter->reader, 0, size, filter->data)) != S_OK)
    {
        WARN("Failed to read %Iu bytes, hr %#lx.\n", size, hr);
        hr = VFW_E_TYPE_NOT_ACCEPTED;
        goto fail;
    }

    start = skip_id3v2(filter->data, size);
    for (pos = start; pos < size && pos - start <= MPEG_MAX_JUNK && size - pos >= 4; ++pos)
    {
        if (parse_header(filter->data + pos, &first) && frames_follow(filter->data, size, pos, &first))
        {
            found = true;
            break;
        }
    }
    if (!found)
    {
        TRACE("No MPEG audio frames found.\n");
        hr = VFW_E_TYPE_NOT_ACCEPTED;
        goto fail;
    }
    TRACE("MPEG-%s layer %u, %u Hz, %u channels, %u bps, first frame at %Iu.\n",
            first.version == 3 ? "2.5" : first.version == 2 ? "2" : "1", first.layer,
            first.rate, first.channels, first.bitrate, pos);

    if (FAILED(hr = index_frames(filter, size, pos, &first))
            || FAILED(hr = init_media_type(filter, &first)))
        goto fail;

    strmbase_source_init(&filter->source, &filter->filter, L"Audio", &source_ops);
    strmbase_seeking_init(&filter->seek, &seeking_vtbl, mpeg_change_stop, mpeg_change_current, mpeg_change_rate);
    filter->has_source = true;
    filter->seek.llDuration = filter->seek.llStop = filter->frames[filter->frame_count - 1].end;
    filter->seek.llCurrent = 0;
    TRACE("%Iu frames, %s.\n", filter->frame_count, debugstr_time(filter->seek.llDuration));
    strmbase_dump_media_type(&filter->mt);
    BaseFilterImpl_IncrementPinVersion(&filter->filter);
    return S_OK;

fail:
    remove_source(filter);
    IAsyncReader_Release(filter->reader);
    filter->reader = NULL;
    return hr;
}

static void mpeg_sink_disconnect(struct strmbase_sink *iface)
{
    struct mpeg_splitter *filter = impl_from_strmbase_sink(iface);

    remove_source(filter);
    IAsyncReader_Release(filter->reader);
    filter->reader = NULL;
}

/* Without the registry's byte patterns the file source may offer no subtype,
 * so the frames are checked on connection instead. */
static HRESULT mpeg_sink_query_accept(struct strmbase_pin *iface, const AM_MEDIA_TYPE *mt)
{
    if (IsEqualGUID(&mt->majortype, &MEDIATYPE_Stream)
            && (IsEqualGUID(&mt->subtype, &MEDIASUBTYPE_MPEG1Audio) || IsEqualGUID(&mt->subtype, &GUID_NULL)))
        return S_OK;
    return S_FALSE;
}

static const struct strmbase_sink_ops sink_ops =
{
    .base.pin_query_accept = mpeg_sink_query_accept,
    .sink_connect = mpeg_sink_connect,
    .sink_disconnect = mpeg_sink_disconnect,
};

static HRESULT source_query_interface(struct strmbase_pin *iface, REFIID iid, void **out)
{
    struct mpeg_splitter *filter = impl_from_strmbase_pin(iface);

    if (IsEqualGUID(iid, &IID_IMediaSeeking))
        *out = &filter->seek.IMediaSeeking_iface;
    else
        return E_NOINTERFACE;

    IUnknown_AddRef((IUnknown *)*out);
    return S_OK;
}

static HRESULT source_query_accept(struct strmbase_pin *iface, const AM_MEDIA_TYPE *mt)
{
    struct mpeg_splitter *filter = impl_from_strmbase_pin(iface);

    if (!IsEqualGUID(&mt->majortype, &filter->mt.majortype) || !IsEqualGUID(&mt->subtype, &filter->mt.subtype))
        return S_FALSE;
    if (IsEqualGUID(&mt->formattype, &GUID_NULL))
        return S_OK;
    if (!IsEqualGUID(&mt->formattype, &filter->mt.formattype) || mt->cbFormat != filter->mt.cbFormat
            || memcmp(mt->pbFormat, filter->mt.pbFormat, mt->cbFormat))
        return S_FALSE;
    return S_OK;
}

static HRESULT source_get_media_type(struct strmbase_pin *iface, unsigned int index, AM_MEDIA_TYPE *mt)
{
    struct mpeg_splitter *filter = impl_from_strmbase_pin(iface);

    if (index)
        return VFW_S_NO_MORE_ITEMS;
    return CopyMediaType(mt, &filter->mt);
}

static HRESULT WINAPI source_decide_buffer_size(struct strmbase_source *iface,
        IMemAllocator *allocator, ALLOCATOR_PROPERTIES *props)
{
    struct mpeg_splitter *filter = impl_from_strmbase_pin(&iface->pin);
    ALLOCATOR_PROPERTIES ret_props;

    /* Decoders may hold on to a few samples, as mpg123audiodec keeps two. */
    props->cBuffers = max(props->cBuffers, 8);
    props->cbBuffer = max(props->cbBuffer, filter->max_frame_size);
    props->cbBuffer = max(props->cbBuffer, 1);
    props->cbAlign = max(props->cbAlign, 1);
    return IMemAllocator_SetProperties(allocator, props, &ret_props);
}

static const struct strmbase_source_ops source_ops =
{
    .base.pin_query_interface = source_query_interface,
    .base.pin_query_accept = source_query_accept,
    .base.pin_get_media_type = source_get_media_type,
    .pfnAttemptConnection = BaseOutputPinImpl_AttemptConnection,
    .pfnDecideAllocator = BaseOutputPinImpl_DecideAllocator,
    .pfnDecideBufferSize = source_decide_buffer_size,
};

/* Start at the frame playing at time. */
static void seek_frames(struct mpeg_splitter *filter, REFERENCE_TIME time)
{
    size_t i = 0;

    while (i < filter->frame_count && filter->frames[i].end <= time)
        ++i;
    filter->next = i;
    filter->need_segment = true;
    filter->discontinuity = true;
    filter->eos = false;
}

static HRESULT WINAPI seeking_QueryInterface(IMediaSeeking *iface, REFIID iid, void **out)
{
    struct mpeg_splitter *filter = impl_from_IMediaSeeking(iface);
    return IPin_QueryInterface(&filter->source.pin.IPin_iface, iid, out);
}

static ULONG WINAPI seeking_AddRef(IMediaSeeking *iface)
{
    struct mpeg_splitter *filter = impl_from_IMediaSeeking(iface);
    return IPin_AddRef(&filter->source.pin.IPin_iface);
}

static ULONG WINAPI seeking_Release(IMediaSeeking *iface)
{
    struct mpeg_splitter *filter = impl_from_IMediaSeeking(iface);
    return IPin_Release(&filter->source.pin.IPin_iface);
}

static HRESULT WINAPI seeking_SetPositions(IMediaSeeking *iface,
        LONGLONG *current, DWORD current_flags, LONGLONG *stop, DWORD stop_flags)
{
    struct mpeg_splitter *filter = impl_from_IMediaSeeking(iface);
    bool flush = !(current_flags & AM_SEEKING_NoFlush) && filter->source.pin.peer;

    TRACE("filter %p, current %s, current_flags %#lx, stop %s, stop_flags %#lx.\n",
            filter, current ? debugstr_time(*current) : "<null>", current_flags,
            stop ? debugstr_time(*stop) : "<null>", stop_flags);

    /* Stopped, mpeg_init_stream() starts from the new position. */
    if (filter->filter.state == State_Stopped || !filter->thread)
    {
        SourceSeekingImpl_SetPositions(iface, current, current_flags, stop, stop_flags);
        return S_OK;
    }

    /* A new stop time alone leaves the stream where it is. */
    if ((current_flags & AM_SEEKING_PositioningBitsMask) == AM_SEEKING_NoPositioning)
    {
        EnterCriticalSection(&filter->flushing_cs);
        SourceSeekingImpl_SetPositions(iface, current, current_flags, stop, stop_flags);
        LeaveCriticalSection(&filter->flushing_cs);
        return S_OK;
    }

    /* Flushing makes downstream refuse and release samples, so the streaming
     * thread finishes its frame and lets go of flushing_cs. */
    if (flush)
        IPin_BeginFlush(filter->source.pin.peer);
    EnterCriticalSection(&filter->flushing_cs);
    SourceSeekingImpl_SetPositions(iface, current, current_flags, stop, stop_flags);
    if (flush)
        IPin_EndFlush(filter->source.pin.peer);
    seek_frames(filter, filter->seek.llCurrent);
    LeaveCriticalSection(&filter->flushing_cs);
    WakeConditionVariable(&filter->eos_cv);
    return S_OK;
}

static const IMediaSeekingVtbl seeking_vtbl =
{
    seeking_QueryInterface,
    seeking_AddRef,
    seeking_Release,
    SourceSeekingImpl_GetCapabilities,
    SourceSeekingImpl_CheckCapabilities,
    SourceSeekingImpl_IsFormatSupported,
    SourceSeekingImpl_QueryPreferredFormat,
    SourceSeekingImpl_GetTimeFormat,
    SourceSeekingImpl_IsUsingTimeFormat,
    SourceSeekingImpl_SetTimeFormat,
    SourceSeekingImpl_GetDuration,
    SourceSeekingImpl_GetStopPosition,
    SourceSeekingImpl_GetCurrentPosition,
    SourceSeekingImpl_ConvertTimeFormat,
    seeking_SetPositions,
    SourceSeekingImpl_GetPositions,
    SourceSeekingImpl_GetAvailable,
    SourceSeekingImpl_SetRate,
    SourceSeekingImpl_GetRate,
    SourceSeekingImpl_GetPreroll,
};

static HRESULT send_frame(struct mpeg_splitter *filter, const struct mpeg_frame *frame)
{
    REFERENCE_TIME start, end;
    IMediaSample *sample;
    BYTE *data;
    HRESULT hr;

    if (filter->need_segment)
    {
        if (FAILED(hr = IPin_NewSegment(filter->source.pin.peer,
                filter->seek.llCurrent, filter->seek.llStop, filter->seek.dRate)))
            WARN("Failed to deliver new segment, hr %#lx.\n", hr);
        filter->need_segment = false;
    }

    if (FAILED(hr = IMemAllocator_GetBuffer(filter->source.pAllocator, &sample, NULL, NULL, 0)))
    {
        if (hr != VFW_E_NOT_COMMITTED)  /* not stopping */
            WARN("Failed to get a sample, hr %#lx.\n", hr);
        return hr;
    }
    if (IMediaSample_GetSize(sample) < frame->size)
    {
        ERR("A %lu byte frame does not fit a %ld byte sample.\n", frame->size, IMediaSample_GetSize(sample));
        IMediaSample_Release(sample);
        return S_OK;
    }
    IMediaSample_GetPointer(sample, &data);
    memcpy(data, filter->data + frame->offset, frame->size);
    IMediaSample_SetActualDataLength(sample, frame->size);

    start = frame->start - filter->seek.llCurrent;
    end = frame->end - filter->seek.llCurrent;
    if (filter->seek.dRate > 0.0 && filter->seek.dRate != 1.0)
    {
        start = (REFERENCE_TIME)(start / filter->seek.dRate);
        end = (REFERENCE_TIME)(end / filter->seek.dRate);
    }
    IMediaSample_SetTime(sample, &start, &end);
    IMediaSample_SetMediaTime(sample, NULL, NULL);
    IMediaSample_SetSyncPoint(sample, TRUE);
    IMediaSample_SetPreroll(sample, frame->end <= filter->seek.llCurrent);
    IMediaSample_SetDiscontinuity(sample, filter->discontinuity);
    filter->discontinuity = false;

    hr = IMemInputPin_Receive(filter->source.pMemInputPin, sample);
    IMediaSample_Release(sample);
    return hr;
}

static DWORD CALLBACK stream_thread(void *arg)
{
    struct mpeg_splitter *filter = arg;

    TRACE("Starting streaming thread for filter %p.\n", filter);

    for (;;)
    {
        EnterCriticalSection(&filter->flushing_cs);

        if (!filter->streaming)
        {
            LeaveCriticalSection(&filter->flushing_cs);
            break;
        }

        if (filter->eos)
        {
            SleepConditionVariableCS(&filter->eos_cv, &filter->flushing_cs, INFINITE);
            LeaveCriticalSection(&filter->flushing_cs);
            continue;
        }

        if (filter->next < filter->frame_count
                && (!filter->seek.llStop || filter->frames[filter->next].start < filter->seek.llStop))
        {
            HRESULT hr = send_frame(filter, &filter->frames[filter->next++]);

            if (hr == S_FALSE || hr == VFW_E_WRONG_STATE || hr == VFW_E_NOT_COMMITTED)
            {
                /* Downstream is flushing or stopping: wait for the seek or the stop. */
                filter->eos = true;
            }
            else if (FAILED(hr))
            {
                /* End the stream rather than leave the graph waiting for it. */
                WARN("Streaming failed, hr %#lx; ending the stream.\n", hr);
                IPin_EndOfStream(filter->source.pin.peer);
                filter->eos = true;
            }
        }
        else
        {
            TRACE("End of stream.\n");
            IPin_EndOfStream(filter->source.pin.peer);
            filter->eos = true;
        }

        LeaveCriticalSection(&filter->flushing_cs);
    }

    TRACE("Streaming stopped; exiting.\n");
    return 0;
}

static struct strmbase_pin *mpeg_get_pin(struct strmbase_filter *iface, unsigned int index)
{
    struct mpeg_splitter *filter = impl_from_strmbase_filter(iface);

    if (!index)
        return &filter->sink.pin;
    if (index == 1 && filter->has_source)
        return &filter->source.pin;
    return NULL;
}

static void mpeg_destroy(struct strmbase_filter *iface)
{
    struct mpeg_splitter *filter = impl_from_strmbase_filter(iface);

    /* Disconnecting the input pin removes the output pin. */
    if (filter->sink.pin.peer)
    {
        IPin_Disconnect(filter->sink.pin.peer);
        IPin_Disconnect(&filter->sink.pin.IPin_iface);
    }
    if (filter->reader)
        IAsyncReader_Release(filter->reader);
    filter->reader = NULL;
    remove_source(filter);

    filter->flushing_cs.DebugInfo->Spare[0] = 0;
    DeleteCriticalSection(&filter->flushing_cs);
    strmbase_sink_cleanup(&filter->sink);
    strmbase_filter_cleanup(&filter->filter);
    free(filter);
}

static HRESULT mpeg_init_stream(struct strmbase_filter *iface)
{
    struct mpeg_splitter *filter = impl_from_strmbase_filter(iface);
    HRESULT hr;

    if (!filter->reader || !filter->has_source || !filter->source.pin.peer)
        return S_OK;

    if (FAILED(hr = IMemAllocator_Commit(filter->source.pAllocator)))
        ERR("Failed to commit allocator, hr %#lx.\n", hr);
    /* DirectShow keeps the seek position, and resets to it when it goes
     * from stopped to paused. */
    seek_frames(filter, filter->seek.llCurrent);
    filter->streaming = true;
    filter->thread = CreateThread(NULL, 0, stream_thread, filter, 0, NULL);
    return S_OK;
}

static HRESULT mpeg_cleanup_stream(struct strmbase_filter *iface)
{
    struct mpeg_splitter *filter = impl_from_strmbase_filter(iface);

    if (!filter->thread)
        return S_OK;

    /* Decommitting releases a thread waiting for a sample. */
    IMemAllocator_Decommit(filter->source.pAllocator);
    EnterCriticalSection(&filter->flushing_cs);
    filter->streaming = false;
    LeaveCriticalSection(&filter->flushing_cs);
    WakeConditionVariable(&filter->eos_cv);
    WaitForSingleObject(filter->thread, INFINITE);
    CloseHandle(filter->thread);
    filter->thread = NULL;
    return S_OK;
}

static const struct strmbase_filter_ops filter_ops =
{
    .filter_get_pin = mpeg_get_pin,
    .filter_destroy = mpeg_destroy,
    .filter_init_stream = mpeg_init_stream,
    .filter_cleanup_stream = mpeg_cleanup_stream,
};

HRESULT native_mpeg_audio_splitter_create(IUnknown *outer, IUnknown **out)
{
    struct mpeg_splitter *object;

    if (!(object = calloc(1, sizeof(*object))))
        return E_OUTOFMEMORY;

    strmbase_filter_init(&object->filter, outer, &CLSID_MPEG1Splitter, &filter_ops);
    strmbase_sink_init(&object->sink, &object->filter, L"Input", &sink_ops, NULL);
    InitializeCriticalSectionEx(&object->flushing_cs, 0, RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO);
    object->flushing_cs.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": mpeg_splitter.flushing_cs");
    InitializeConditionVariable(&object->eos_cv);

    TRACE("Created MPEG audio splitter %p.\n", object);
    *out = &object->filter.IUnknown_inner;
    return S_OK;
}
