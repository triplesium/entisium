# Standalone Luau host

This directory contains the `entisium-luau-host` native process entrypoint.
It loads project scripts, forwards host requests over stdin/stdout, and supports
bounded execution and cancellation.

The script-facing SDK lives in [scripting/libraries/playtest](../../scripting/libraries/playtest/).
See the [Playtest guide](../../scripting/docs/playtest.md) for running and writing
tests, or the [Scripting index](../../scripting/README.md) for libraries and examples.

[DevKit](../../devkit/src/playtest/) starts this host with the project source root,
test entry and SDK source directory, and coordinates it with the game runtime.
