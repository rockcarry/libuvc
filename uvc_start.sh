#!/bin/sh

insmod /lib/modules/media.ko
insmod /lib/modules/videodev.ko
insmod /lib/modules/v4l2-common.ko
insmod /lib/modules/videobuf2-core.ko
insmod /lib/modules/videobuf2-memops.ko
insmod /lib/modules/videobuf2-vmalloc.ko
insmod /lib/modules/ms_usb.ko
insmod /lib/modules/usb-common.ko
insmod /lib/modules/usbcore.ko
insmod /lib/modules/udc-core.ko
insmod /lib/modules/udc-msb250x.ko
insmod /lib/modules/phy-ms-usb.ko
insmod /lib/modules/libcomposite.ko
insmod /lib/modules/usb_f_uac1.ko
insmod /lib/modules/usb_f_uvc.ko
insmod /lib/modules/g_webcam.ko bulk_streaming_ep=0

