#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <ctype.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/usb/ch9.h>
#include <linux/videodev2.h>
#include "linux/video.h"
#include "linux/uvc.h"

#define ARRAY_SIZE(a) ((sizeof(a) / sizeof(a[0])))
#define clamp(val, min, max) ({                 \
        typeof(val) __val = (val);              \
        typeof(min) __min = (min);              \
        typeof(max) __max = (max);              \
        (void) (&__val == &__min);              \
        (void) (&__val == &__max);              \
        __val = __val < __min ? __min: __val;   \
        __val > __max ? __max: __val; })

struct uvc_device {
    int             vibrate;

    #define FLAG_EXIT_ALL    (1 << 0)
    #define FLAG_VENC_INITED (1 << 1)
    #define FLAG_REQUEST_IDR (1 << 2)
    uint32_t        status;

    int             fd;
    struct uvc_streaming_control probe ;
    struct uvc_streaming_control commit;

    int             streamon;
    int             control;
    unsigned int    fcc;
    int             width;
    int             height;
    int             vfrate;
    int             maxfsize;

    void          **mem;
    unsigned int    nbufs;
    unsigned int    bufsize;
    unsigned int    bulk;
    uint8_t         color;

    int             vsem;
    uint8_t        *vbuf;
    int             vlen;
    int             yoff;
    int             uoff;
    pthread_mutex_t mutex;
    pthread_cond_t  cond;
    pthread_t  encthread;
    pthread_t  uvcthread;
};

static void* main_video_capture_proc(void *argv)
{
    struct uvc_device *dev     = (struct uvc_device*)argv;

    while (!(dev->status & FLAG_EXIT_ALL)) {
        if (!dev->streamon) {
            usleep(100*1000);
            continue;
        }

        //++ reinit video main stream
        if (!(dev->status & FLAG_VENC_INITED)) {
            dev->status |= FLAG_VENC_INITED;
            // todo..
        }
        //-- reinit video main stream

        if (dev->status & FLAG_REQUEST_IDR) {
            dev->status &= ~FLAG_REQUEST_IDR;
            // todo..
        }

        if (dev->fcc == V4L2_PIX_FMT_NV12) {
            // todo...
        } else {
            // todo...
        }
    }

    return NULL;
}
//-- 313e platform

static struct uvc_device *
uvc_open(const char *devname)
{
    struct uvc_device *dev;
    struct v4l2_capability cap;
    int ret;
    int fd;

    fd = open(devname, O_RDWR);
    if (1) {
        close(fd);
        fd = open(devname, O_RDWR);
    }
    if (fd == -1) {
        printf("v4l2 open failed: %s (%d)\n", strerror(errno), errno);
        return NULL;
    }

    printf("open succeeded, file descriptor = %d\n", fd);

    ret = ioctl(fd, VIDIOC_QUERYCAP, &cap);
    if (ret < 0) {
        printf("unable to query device: %s (%d)\n", strerror(errno), errno);
        close(fd);
        return NULL;
    }

    printf("device is %s on bus %s\n", cap.card, cap.bus_info);

    dev = calloc(1, sizeof *dev);
    if (dev == NULL) {
        close(fd);
        return NULL;
    }

    dev->fd = fd;
    return dev;
}

static void
uvc_close(struct uvc_device *dev)
{
    close(dev->fd);
    free(dev->mem);
    free(dev);
}

/* ---------------------------------------------------------------------------
 * Video streaming
 */

static void
uvc_video_fill_buffer(struct uvc_device *dev, struct v4l2_buffer *buf)
{
    int len, ncopy;

    pthread_mutex_lock(&dev->mutex);
    while (dev->vsem == 0 && !(dev->status & FLAG_EXIT_ALL)) pthread_cond_wait(&dev->cond, &dev->mutex);
    pthread_mutex_unlock(&dev->mutex);
    if (dev->status & FLAG_EXIT_ALL) return;

    if (dev->fcc == V4L2_PIX_FMT_NV12) {
        memcpy(dev->mem[buf->index] + 0                       , dev->vbuf + dev->yoff, dev->width * dev->height / 1);
        memcpy(dev->mem[buf->index] + dev->width * dev->height, dev->vbuf + dev->uoff, dev->width * dev->height / 2);
        buf->bytesused = dev->maxfsize;
    } else {
        len   = dev->vlen;
        ncopy = len < dev->maxfsize ? len : dev->maxfsize;
        memcpy(dev->mem[buf->index], dev->vbuf, ncopy);
        buf->bytesused = ncopy;
    }

    pthread_mutex_lock(&dev->mutex);
    dev->vsem = 0;
    pthread_cond_signal(&dev->cond);
    pthread_mutex_unlock(&dev->mutex);
}

static int
uvc_video_process(struct uvc_device *dev)
{
    struct v4l2_buffer buf;
    int    ret;

    if (!dev->streamon) {
        usleep(100*1000);
        return -1;
    }

    memset(&buf, 0, sizeof buf);
    buf.type   = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    buf.memory = V4L2_MEMORY_MMAP;

    if ((ret = ioctl(dev->fd, VIDIOC_DQBUF, &buf)) < 0) {
        printf("unable to dequeue buffer: %s (%d).\n", strerror(errno), errno);
        return ret;
    }

    uvc_video_fill_buffer(dev, &buf);

    if ((ret = ioctl(dev->fd, VIDIOC_QBUF, &buf)) < 0) {
        printf("unable to requeue buffer: %s (%d).\n", strerror(errno), errno);
        return ret;
    }

    return 0;
}

static int
uvc_video_reqbufs(struct uvc_device *dev, int nbufs)
{
    struct v4l2_requestbuffers rb;
    struct v4l2_buffer buf;
    unsigned int i;
    int ret;

    for (i=0; i<dev->nbufs; ++i)
        munmap(dev->mem[i], dev->bufsize);

    free(dev->mem);
    dev->mem   = 0;
    dev->nbufs = 0;

    memset(&rb, 0, sizeof rb);
    rb.count  = nbufs;
    rb.type   = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    rb.memory = V4L2_MEMORY_MMAP;

    ret = ioctl(dev->fd, VIDIOC_REQBUFS, &rb);
    if (ret < 0) {
        printf("unable to allocate buffers: %s (%d).\n",
               strerror(errno), errno);
        return ret;
    }

    printf("%u buffers allocated.\n", rb.count);

    /* Map the buffers. */
    dev->mem = malloc(rb.count * sizeof dev->mem[0]);

    for (i=0; i<rb.count; ++i) {
        memset(&buf, 0, sizeof buf);
        buf.index  = i;
        buf.type   = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        buf.memory = V4L2_MEMORY_MMAP;
        ret = ioctl(dev->fd, VIDIOC_QUERYBUF, &buf);
        if (ret < 0) {
            printf("unable to query buffer %u: %s (%d).\n", i,
                strerror(errno), errno);
            return -1;
        }
        printf("length: %u offset: %u\n", buf.length, buf.m.offset);

        dev->mem[i] = mmap(0, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, dev->fd, buf.m.offset);
        if (dev->mem[i] == MAP_FAILED) {
            printf("unable to map buffer %u: %s (%d)\n", i,
                   strerror(errno), errno);
            return -1;
        }
        printf("buffer %u mapped at address %p.\n", i, dev->mem[i]);
    }

    dev->bufsize = buf.length;
    dev->nbufs   = rb.count;
    return 0;
}

static int
uvc_video_stream(struct uvc_device *dev, int enable)
{
    struct v4l2_buffer buf;
    int type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    int ret, i;
    if (enable) {
        printf("starting video stream.\n");
        dev->status  &= ~FLAG_VENC_INITED;
        dev->streamon = 1;
        for (i=0; i<dev->nbufs; ++i) {
            memset(&buf, 0, sizeof buf);
            buf.index  = i;
            buf.type   = V4L2_BUF_TYPE_VIDEO_OUTPUT;
            buf.memory = V4L2_MEMORY_MMAP;
            uvc_video_fill_buffer(dev, &buf);
            printf("queueing buffer %u.\n", i);
            if ((ret = ioctl(dev->fd, VIDIOC_QBUF, &buf)) < 0) {
                printf("unable to queue buffer: %s (%d).\n", strerror(errno), errno);
                break;
            }
        }
        ret = ioctl(dev->fd, VIDIOC_STREAMON, &type);
    } else {
        printf("stopping video stream.\n");
        dev->streamon = 0;
        ret = ioctl(dev->fd, VIDIOC_STREAMOFF, &type);
    }
    return ret;
}

static int
uvc_video_set_format(struct uvc_device *dev)
{
    struct v4l2_format fmt;
    int    ret;
    printf("setting format to 0x%08x %ux%u\n",
           dev->fcc, dev->width, dev->height);

    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    if ((ret = ioctl(dev->fd, VIDIOC_G_FMT, &fmt)) < 0) {
        printf("unable to get format: %s (%d).\n", strerror(errno), errno);
    }

    fmt.fmt.pix.width       = dev->width;
    fmt.fmt.pix.height      = dev->height;
    fmt.fmt.pix.pixelformat = dev->fcc;
    fmt.fmt.pix.field       = V4L2_FIELD_NONE;
    switch (dev->fcc) {
    case V4L2_PIX_FMT_NV12:
        fmt.fmt.pix.sizeimage = dev->width * dev->height * 1.5;
        break;
    case V4L2_PIX_FMT_MJPEG:
        fmt.fmt.pix.sizeimage = dev->width * dev->height * 1.5 / 3;
        break;
    case v4l2_fourcc('H','2','6','4'):
        fmt.fmt.pix.sizeimage = dev->width * dev->height * 1.5 / 4;
        break;
    case v4l2_fourcc('H','2','6','5'):
        fmt.fmt.pix.sizeimage = dev->width * dev->height * 1.5 / 5;
        break;
    }
    if ((ret = ioctl(dev->fd, VIDIOC_S_FMT, &fmt)) < 0) {
        printf("unable to set format: %s (%d).\n", strerror(errno), errno);
    }
    return ret;
}

static int
uvc_video_init(struct uvc_device *dev __attribute__((__unused__)))
{
    return 0;
}

/* ---------------------------------------------------------------------------
 * Request processing
 */
struct uvc_frame_info {
    unsigned int width;
    unsigned int height;
    unsigned int intervals[8];
};

struct uvc_format_info {
    unsigned int fcc;
    const struct uvc_frame_info *frames;
};

static const struct uvc_frame_info uvc_frames_nv12[] = {
    { 320 , 240 , { 1000000000 / 25 / 100, 0 } },
    { 640 , 480 , { 1000000000 / 25 / 100, 0 } },
    { 1280, 720 , { 1000000000 / 15 / 100, 0 } },
    {},
};

static const struct uvc_frame_info uvc_frames_mjpeg[] = {
    { 640 , 480 , { 1000000000 / 25 / 100, 0 } },
    { 1280, 720 , { 1000000000 / 25 / 100, 0 } },
    { 1920, 1080, { 1000000000 / 25 / 100, 0 } },
    {},
};

static const struct uvc_frame_info uvc_frames_h264[] = {
    { 640 , 480 , { 1000000000 / 25 / 100, 0 } },
    { 1280, 720 , { 1000000000 / 25 / 100, 0 } },
    { 1920, 1080, { 1000000000 / 25 / 100, 0 } },
    {},
};

static const struct uvc_frame_info uvc_frames_h265[] = {
    { 640 , 480 , { 1000000000 / 25 / 100, 0 } },
    { 1280, 720 , { 1000000000 / 25 / 100, 0 } },
    { 1920, 1080, { 1000000000 / 25 / 100, 0 } },
    {},
};

static const struct uvc_format_info uvc_formats[] = {
    { V4L2_PIX_FMT_NV12 , uvc_frames_nv12  },
    { V4L2_PIX_FMT_MJPEG, uvc_frames_mjpeg },
    { v4l2_fourcc('H','2','6','4'), uvc_frames_h264 },
    { v4l2_fourcc('H','2','6','5'), uvc_frames_h265 },
};

static void
uvc_fill_streaming_control(struct uvc_device *dev,
                           struct uvc_streaming_control *ctrl,
                           int iframe, int iformat)
{
    const struct uvc_format_info *format;
    const struct uvc_frame_info  *frame ;
    unsigned int nframes;

    if (iformat < 0) iformat = ARRAY_SIZE(uvc_formats) + iformat;
    if (iformat < 0 || iformat >= (int)ARRAY_SIZE(uvc_formats)) return;
    format = &uvc_formats[iformat];

    nframes = 0;
    while (format->frames[nframes].width != 0) {
        ++nframes;
    }

    if (iframe < 0) iframe = nframes + iframe;
    if (iframe < 0 || iframe >= (int)nframes) return;
    frame = &format->frames[iframe];

    memset(ctrl, 0, sizeof(*ctrl));
    ctrl->bmHint          = 1;
    ctrl->bFormatIndex    = iformat + 1;
    ctrl->bFrameIndex     = iframe  + 1;
    ctrl->dwFrameInterval = frame->intervals[0];
    switch (format->fcc) {
    case V4L2_PIX_FMT_NV12:
        ctrl->dwMaxVideoFrameSize = frame->width * frame->height * 1.5;
        break;
    case V4L2_PIX_FMT_MJPEG:
        ctrl->dwMaxVideoFrameSize = frame->width * frame->height * 1.5 / 3;
        break;
    case v4l2_fourcc('H','2','6','4'):
        ctrl->dwMaxVideoFrameSize = frame->width * frame->height * 1.5 / 4;
        break;
    case v4l2_fourcc('H','2','6','5'):
        ctrl->dwMaxVideoFrameSize = frame->width * frame->height * 1.5 / 5;
        break;
    }
    ctrl->dwMaxPayloadTransferSize = 1024; /* TODO this should be filled by the driver. */
    ctrl->bmFramingInfo    = 3;
    ctrl->bPreferedVersion = 1;
    ctrl->bMaxVersion      = 1;
}

static void
uvc_events_process_standard(struct uvc_device *dev, struct usb_ctrlrequest *ctrl,
                            struct uvc_request_data *resp)
{
    printf("standard request\n");
    (void)dev;
    (void)ctrl;
    (void)resp;
}

static void
uvc_events_process_control(struct uvc_device *dev, uint8_t req, uint8_t cs,
                           struct uvc_request_data *resp)
{
    printf("control request (req %02x cs %02x)\n", req, cs);
    //++ do not remove these code
    if (resp->length < 0) {
        resp->data[0] = 0x5;
        resp->length  = 1;
    }
    //-- do not remove these code
}

static void
uvc_events_process_streaming(struct uvc_device *dev, uint8_t req, uint8_t cs,
                             struct uvc_request_data *resp)
{
    struct uvc_streaming_control *ctrl;

    printf("streaming request (req %02x cs %02x)\n", req, cs);
    if (cs != UVC_VS_PROBE_CONTROL && cs != UVC_VS_COMMIT_CONTROL)
        return;

    ctrl = (struct uvc_streaming_control *)&resp->data;
    resp->length = sizeof(*ctrl);

    switch (req) {
    case UVC_SET_CUR:
        dev->control = cs;
        resp->length = 19;
        break;
    case UVC_GET_CUR:
        if (cs == UVC_VS_PROBE_CONTROL) {
            memcpy(ctrl, &dev->probe, sizeof(*ctrl));
        } else {
            memcpy(ctrl, &dev->commit, sizeof(*ctrl));
        }
        break;
    case UVC_GET_MIN:
    case UVC_GET_MAX:
    case UVC_GET_DEF:
        uvc_fill_streaming_control(dev, ctrl, req == UVC_GET_MAX ? -1 : 0,
                                   req == UVC_GET_MAX ? -1 : 0);
        break;
    case UVC_GET_RES:
        memset(ctrl, 0, sizeof(*ctrl));
        break;
    case UVC_GET_LEN:
        resp->data[0] = 0x00;
        resp->data[1] = 0x22;
        resp->length  = 2;
        break;
    case UVC_GET_INFO:
        resp->data[0] = 0x03;
        resp->length  = 1;
        break;
    }
}

static void
uvc_events_process_class(struct uvc_device *dev, struct usb_ctrlrequest *ctrl,
             struct uvc_request_data *resp)
{
    if ((ctrl->bRequestType & USB_RECIP_MASK) != USB_RECIP_INTERFACE)
        return;

    switch (ctrl->wIndex & 0xff) {
    case UVC_INTF_CONTROL:
        uvc_events_process_control(dev, ctrl->bRequest, ctrl->wValue >> 8, resp);
        break;
    case UVC_INTF_STREAMING:
        uvc_events_process_streaming(dev, ctrl->bRequest, ctrl->wValue >> 8, resp);
        break;
    }
}

static void
uvc_events_process_setup(struct uvc_device *dev, struct usb_ctrlrequest *ctrl,
                         struct uvc_request_data *resp)
{
    dev->control = 0;

    printf("bRequestType %02x bRequest %02x wValue %04x wIndex %04x "
           "wLength %04x\n", ctrl->bRequestType, ctrl->bRequest,
           ctrl->wValue, ctrl->wIndex, ctrl->wLength);

    switch (ctrl->bRequestType & USB_TYPE_MASK) {
    case USB_TYPE_STANDARD:
        uvc_events_process_standard(dev, ctrl, resp);
        break;
    case USB_TYPE_CLASS:
        uvc_events_process_class(dev, ctrl, resp);
        break;
    }
}

static void
uvc_events_process_data(struct uvc_device *dev, struct uvc_request_data *data)
{
    struct uvc_streaming_control *target;
    struct uvc_streaming_control *ctrl;
    const struct uvc_format_info *format;
    const struct uvc_frame_info  *frame;
    const unsigned int *interval;
    unsigned int iformat, iframe;
    unsigned int nframes;

    switch (dev->control) {
    case UVC_VS_PROBE_CONTROL:
        printf("setting probe control, length = %d\n", data->length);
        target = &dev->probe;
        break;
    case UVC_VS_COMMIT_CONTROL:
        printf("setting commit control, length = %d\n", data->length);
        target = &dev->commit;
        break;
    default:
        printf("setting unknown control, length = %d\n", data->length);
        return;
    }

    ctrl    = (struct uvc_streaming_control *)&data->data;
    iformat = clamp((unsigned int)ctrl->bFormatIndex, 1U,
                    (unsigned int)ARRAY_SIZE(uvc_formats));
    format  = &uvc_formats[iformat-1];

    nframes = 0;
    while (format->frames[nframes].width != 0) {
        ++nframes;
    }

    iframe   = clamp((unsigned int)ctrl->bFrameIndex, 1U, nframes);
    frame    = &format->frames[iframe-1];
    interval = frame->intervals;
    while (interval[0] < ctrl->dwFrameInterval && interval[1]) {
        ++interval;
    }

    target->bFormatIndex = iformat;
    target->bFrameIndex  = iframe;
    switch (format->fcc) {
    case V4L2_PIX_FMT_NV12:
        target->dwMaxVideoFrameSize = frame->width * frame->height * 1.5;
        break;
    case V4L2_PIX_FMT_MJPEG:
        target->dwMaxVideoFrameSize = frame->width * frame->height * 1.5 / 3;
        break;
    case v4l2_fourcc('H','2','6','4'):
        target->dwMaxVideoFrameSize = frame->width * frame->height * 1.5 / 4;
        break;
    case v4l2_fourcc('H','2','6','5'):
        target->dwMaxVideoFrameSize = frame->width * frame->height * 1.5 / 5;
        break;
    }
    target->dwFrameInterval = *interval;

    if (dev->control == UVC_VS_COMMIT_CONTROL) {
        dev->fcc     = format->fcc;
        dev->width   = frame->width;
        dev->height  = frame->height;
        dev->vfrate  = ((int)(1.0/target->dwFrameInterval*10000000));
        dev->maxfsize= target->dwMaxVideoFrameSize;
        switch (dev->fcc) {
        case v4l2_fourcc('H','2','6','4'):
            switch (dev->width) {
            case 1920: dev->vibrate = 3000*1000; break;
            case 1280: dev->vibrate = 2000*1000; break;
            default  : dev->vibrate = 1000*1000; break;
            }
            break;
        case v4l2_fourcc('H','2','6','5'):
            switch (dev->width) {
            case 1920: dev->vibrate = 3000*1000; break;
            case 1280: dev->vibrate = 2000*1000; break;
            default  : dev->vibrate = 1000*1000; break;
            }
            break;
        case V4L2_PIX_FMT_MJPEG:
            break;
        case V4L2_PIX_FMT_NV12:
            break;
        }
        uvc_video_set_format(dev);
        if (dev->bulk) {
            uvc_video_stream(dev, 1);
        }
    }
}

static void
uvc_events_process(struct uvc_device *dev)
{
    struct v4l2_event       v4l2_event = {};
    struct uvc_event       *uvc_event  = (void*)&v4l2_event.u.data;
    struct uvc_request_data resp;
    int    ret;

    ret = ioctl(dev->fd, VIDIOC_DQEVENT, &v4l2_event);
    if (ret < 0) {
        printf("VIDIOC_DQEVENT failed: %s (%d)\n", strerror(errno), errno);
        return;
    }

    memset(&resp, 0, sizeof resp);
    resp.length = -EL2HLT;

    switch (v4l2_event.type) {
    case UVC_EVENT_CONNECT:
    case UVC_EVENT_DISCONNECT:
        return;
    case UVC_EVENT_SETUP:
        uvc_events_process_setup(dev, &uvc_event->req, &resp);
        break;
    case UVC_EVENT_DATA:
        uvc_events_process_data(dev, &uvc_event->data);
        return;
    case UVC_EVENT_STREAMON:
        uvc_video_reqbufs(dev, 3);
        uvc_video_stream (dev, 1);
        dev->status |= FLAG_REQUEST_IDR;
        return;
    case UVC_EVENT_STREAMOFF:
        uvc_video_stream (dev, 0);
        uvc_video_reqbufs(dev, 0);
        return;
    }

    ret = ioctl(dev->fd, UVCIOC_SEND_RESPONSE, &resp);
    if (ret < 0) {
        printf("UVCIOC_S_EVENT failed: %s (%d)\n", strerror(errno), errno);
        return;
    }
}

static void
uvc_events_init(struct uvc_device *dev)
{
    struct v4l2_event_subscription sub;
    uvc_fill_streaming_control(dev, &dev->probe , 0, 0);
    uvc_fill_streaming_control(dev, &dev->commit, 0, 0);
    if (dev->bulk) {
        dev->probe .dwMaxPayloadTransferSize = 16 * 1024;
        dev->commit.dwMaxPayloadTransferSize = 16 * 1024;
    }

    memset(&sub, 0, sizeof sub);
    sub.type = UVC_EVENT_SETUP;      ioctl(dev->fd, VIDIOC_SUBSCRIBE_EVENT, &sub);
    sub.type = UVC_EVENT_DATA;       ioctl(dev->fd, VIDIOC_SUBSCRIBE_EVENT, &sub);
    sub.type = UVC_EVENT_STREAMON;   ioctl(dev->fd, VIDIOC_SUBSCRIBE_EVENT, &sub);
    sub.type = UVC_EVENT_STREAMOFF;  ioctl(dev->fd, VIDIOC_SUBSCRIBE_EVENT, &sub);
    sub.type = UVC_EVENT_CONNECT;    ioctl(dev->fd, VIDIOC_SUBSCRIBE_EVENT, &sub);
    sub.type = UVC_EVENT_DISCONNECT; ioctl(dev->fd, VIDIOC_SUBSCRIBE_EVENT, &sub);
    sub.type = UVC_EVENT_FIRST;      ioctl(dev->fd, VIDIOC_SUBSCRIBE_EVENT, &sub);
    sub.type = UVC_EVENT_LAST;       ioctl(dev->fd, VIDIOC_SUBSCRIBE_EVENT, &sub);
}

static void* camuvc_process_proc(void *argv)
{
    struct uvc_device *dev = (struct uvc_device*)argv;
    fd_set fds;
    int    ret;

    FD_ZERO(&fds);
    FD_SET(dev->fd, &fds);
    while (!(dev->status & FLAG_EXIT_ALL)) {
        struct timeval tv;
        fd_set efds = fds;
        fd_set wfds = fds;
        tv.tv_sec   = 1;
        tv.tv_usec  = 0;
        ret = select(dev->fd + 1, NULL, &wfds, &efds, &tv);
        if (ret == -1) {
            printf("select error !\n");
            break;
        }
        if (ret ==  0) {
//          printf("select timeout !\n");
            continue;
        }
        if (FD_ISSET(dev->fd, &efds)) {
//          printf("uvc_events_process\n");
            uvc_events_process(dev);
        }
        if (FD_ISSET(dev->fd, &wfds)) {
//          printf("uvc_video_process\n");
            uvc_video_process(dev);
        }
    }

    return NULL;
}

void* camuvc_init(char *devname)
{
    struct uvc_device *dev;
    int    bulk_mode = 0;

    dev = uvc_open(devname);
    if (dev == NULL) {
        printf("failed to open video device !\n");
        return NULL;
    }
    dev->bulk = bulk_mode;

    uvc_events_init(dev);
    uvc_video_init (dev);

    // create video encode thread & vvc process thread
    pthread_create(&dev->encthread, NULL, main_video_capture_proc, dev);
    pthread_create(&dev->uvcthread, NULL, camuvc_process_proc    , dev);
    return dev;
}

void camuvc_exit(void *ctxt)
{
    struct uvc_device *dev = (struct uvc_device*)ctxt;
    if (!ctxt) return;

    pthread_mutex_lock(&dev->mutex);
    dev->status |= FLAG_EXIT_ALL;
    pthread_cond_signal(&dev->cond);
    pthread_mutex_unlock(&dev->mutex);

    // exit video encode thread
    if (dev->encthread) pthread_join(dev->encthread, NULL);
    if (dev->uvcthread) pthread_join(dev->uvcthread, NULL);

    uvc_close(dev);
}







