# Linux kernel for the Huawei MediaPad T2 8.0 Pro (JDN-L01)

Kernel 3.10.108 for the Huawei MediaPad T2 8.0 Pro / Honor Pad 2
(JDN-L01, codename `jordan`, MSM8939v2), used to build TWRP 3.7.0.

Huawei never released kernel source for this device. This tree is
therefore based on the **kiwi** (Honor 5X) kernel, which shares the SoC:

- Base: https://github.com/mohammadsa9/android_kernel_huawei_kiwi (`lineage-18.1`)
- Which is in turn derived from Huawei's released kiwi kernel source.

All credit for the base tree goes to **@mohammadsa9**, whose kiwi work
made this port possible.

## What was added for JDN-L01

| Commit | Why |
| --- | --- |
| Board DTS + defconfig | Decompiled from the stock RECOVERY.img DTB. Two panel-supply entries with empty `qcom,supply-name` had to go: `of_property_read_string` returns `-EILSEQ` on a zero-length property, which aborts the DSI0 probe and leaves the device with no framebuffer. |
| Novatek touch driver | This unit ships a Novatek digitiser. Its chip ID cannot be read, but it runs its own firmware and reports in the standard Novatek point format, so the driver just powers it, resets it and decodes points. |
| MDSS backlight GPIOs | The backlight sits on three GPIOs no CAF property name covers, so nothing drove them and the brightness node lit nothing. |
| gcc-wrapper python 3 | Builds on a modern host. |

## Building

    export ARCH=arm64 SUBARCH=arm64
    export CROSS_COMPILE=<path>/aarch64-linux-android-4.9/bin/aarch64-linux-android-
    mkdir -p ../kout
    make O=../kout jdn-64_defconfig
    make O=../kout -j$(nproc) Image dtbs

GCC 4.9 is required; this kernel does not build with a modern GCC or clang.

## Device tree

https://github.com/Shaheddan/android_device_huawei_hwjdn

## License

GPLv2, as the Linux kernel.
