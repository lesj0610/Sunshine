# sudovda

`sudovda-ioctl.h` is the driver interface header for the SudoVDA virtual
display driver, taken verbatim from SudoMaker/SudoVDA.

    source  https://github.com/SudoMaker/SudoVDA
    path    Common/Include/sudovda-ioctl.h
    commit  adc5c5a0
    sha256  dd924c9bd3918f75bdc1c1153ed16f23ae1658fde890d76f5f4f35397062dd73

It is the driver's ABI, so it is vendored rather than reimplemented. The code
that talks to the driver lives in `src/platform/windows/virtual_display.cpp`.

Verify with:

    sha256sum third-party/sudovda/sudovda-ioctl.h
