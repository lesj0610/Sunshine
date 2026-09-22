# sudovda

`sudovda-ioctl.h` is the driver interface header for the SudoVDA virtual
display driver, taken verbatim from SudoMaker/SudoVDA.

    source  https://github.com/SudoMaker/SudoVDA
    path    Common/Include/sudovda-ioctl.h
    commit  adc5c5a0
    sha256  dd924c9bd3918f75bdc1c1153ed16f23ae1658fde890d76f5f4f35397062dd73

One byte differs from upstream: a newline was added at the end, because the
repository's lint requires one. Nothing else was touched, and the difference
is visible as a single trailing byte.

    as vendored here
    sha256  807ddbee815906f27148f0f38748b44caa5816b9c6c2f8cc05ccf289abdf95cd

It is the driver's ABI, so it is vendored rather than reimplemented. The code
that talks to the driver lives in `src/platform/windows/virtual_display.cpp`.

Verify against upstream with:

    printf '%s' "$(cat third-party/sudovda/sudovda-ioctl.h)" | sha256sum

or against the copy here with:

    sha256sum third-party/sudovda/sudovda-ioctl.h
