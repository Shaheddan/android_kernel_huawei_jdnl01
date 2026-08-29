# Linux kernel for the Huawei MediaPad T2 8.0 Pro (JDN-L01)

Kernel 3.10.108 for the Huawei MediaPad T2 8.0 Pro / Honor Pad 2
(JDN-L01, codename `jordan`, MSM8939v2). Builds **TWRP 3.7.0** and
**LineageOS 19.1 (Android 12.1)**.

Huawei never released kernel source for this device. This tree is
therefore based on the **kiwi** (Honor 5X) kernel, which shares the SoC:

- Base: https://github.com/mohammadsa9/android_kernel_huawei_kiwi (`lineage-18.1`)
- Which is in turn derived from Huawei's released kiwi kernel source.

All credit for the base tree goes to **@mohammadsa9**, whose kiwi work
made this port possible.

## Branches

| Branch | Contents |
| --- | --- |
| `lineage-19.1` | Current. Everything below. Use this one. |
| `lineage-18.1` | The original fork point, kept so the relationship to the upstream kiwi tree stays visible. Phase 1 (TWRP) only. |

The `lineage-18.1` name is inherited from the kiwi branch this was forked
from; it does not mean the kernel is limited to LineageOS 18.1.

## Phase 1 — bring-up (TWRP)

| Change | Why |
| --- | --- |
| Board DTS + defconfig | Decompiled from the stock RECOVERY.img DTB. Two panel-supply entries with empty `qcom,supply-name` had to go: `of_property_read_string` returns `-EILSEQ` on a zero-length property, which aborts the DSI0 probe and leaves the device with no framebuffer. |
| Novatek touch driver | This unit ships a Novatek digitiser. Its chip ID cannot be read, but it runs its own firmware and reports in the standard Novatek point format, so the driver just powers it, resets it and decodes points. |
| MDSS backlight GPIOs | The backlight sits on three GPIOs no CAF property name covers, so nothing drove them and the brightness node lit nothing. |
| gcc-wrapper python 3 | Builds on a modern host. |

## Phase 2 — LineageOS 19.1

| Change | Why |
| --- | --- |
| **LM36923 backlight driver** (`drivers/video/lm36923.c`) | The panel node declares `bl_ctrl_dcs`, but this panel ignores DCS `0x51`: Huawei's own kernel replaced `mdss_dsi_panel_bklt_dcs()` with a `_pad` variant driving a TI LM36923 at i2c-0 `0x36`. Register map recovered from the stock kernel's strings, init order from aboot, then verified against the chip over i2c before any code was written. Register `0x19` holds brightness bits 10:3 and maps 1:1 onto `bl_level`. |
| **AP3426 light + proximity** | The DTS declares an APDS-9930 and a BH1745 that are not fitted; the real part is a Dyna Image AP34xx at `0x1e`. Adding `"di,ap3426"` as a second compatible binds the in-tree driver unmodified. Tuning values come from the identically wired ql790 reference. |
| **bma2x2 12-bit fix** | `BMA2X2_SENSOR_IDENTIFICATION_ENABLE` compiled out the `>> (16 - bitwidth)` normalisation, so the 12-bit BMA255 emitted raw 16-bit values. The HAL scaled them by the declared resolution and produced ~157 m/s², which the rotation judge rejected as external acceleration. |
| **rmtfs sharedmem address** | A previous commit moved the rmtfs UIO window off Huawei's `0x86700000` to avoid a 384 KB overlap with `modem_adsp_mem`. That overlap is real, but it is also what Huawei's shipping DTB does. At the moved address, a working `rmt_storage` mapping uio0 with the modem live takes an unrecoverable exception (`Bad mode in Error handler`) and the system dies in milliseconds. Restored, and the modem subsystem enabled. |

## Building

    export ARCH=arm64 SUBARCH=arm64
    export CROSS_COMPILE=<path>/aarch64-linux-android-4.9/bin/aarch64-linux-android-
    mkdir -p ../kout
    make O=../kout jdn-64_defconfig
    make O=../kout -j$(nproc) Image dtbs

GCC 4.9 is required; this kernel does not build with a modern GCC or clang.

Within a LineageOS tree it is built by the device tree's
`TARGET_KERNEL_SOURCE`, so `mka bootimage` is the usual route.

## Device tree

https://github.com/Shaheddan/android_device_huawei_hwjdn

- `main` — TWRP 3.7.0 device tree (Phase 1)
- `lineage-19.1` — LineageOS 19.1 device tree

## License

GPLv2, as the Linux kernel.
