# Wii U file I/O

`SDL_IOFromFile()` uses native Cafe FS for both loose SD files and application
content. `SDL_ReadStorageFile()`, `SDL_WriteStorageFile()`, `SDL_LoadFile()`, and
`SDL_SaveFile()` use the same stream backend.

Small reads fill a 64 KiB region cache. Each stream keeps four regions by default
and prefetches the next region while the application consumes cached bytes. A
read that catches up with prefetch waits for that operation and uses its result.
Bulk reads consume any relevant cached/prefetched bytes, then transfer directly
to the caller's buffer without filling successive cache regions. Prefetch uses
positioned async reads on the stream's existing native handle, avoiding a second
synchronous file open on the first cached read. These reads advance the native
file position; subsequent synchronous transfers seek after queued prefetch work.
Writes and close cancel and drain prefetch callbacks before changing or releasing
the file.

Small sequential writes accumulate in an aligned 64 KiB write buffer. Seeking,
reading, flushing, or closing writes out pending bytes; large writes bypass an
empty write buffer. Append writes retain their immediate append behavior. The
backend never pads a file write to the buffer capacity.

Native buffers require 64-byte alignment. Applications can use
`SDL_aligned_alloc(64, size)` and `SDL_aligned_free()` for bulk I/O. Unaligned
requests use temporary storage in batches of at most 1 MiB. Keeping those
batches a multiple of the device block size preserves the original file-offset
alignment; advancing through an unaligned prefix before switching to direct I/O
was measurably slower on hardware. Partial cache lines also use temporary storage.
Aligned requests use the caller's size; the native FS layer handles device
transaction sizing. Beyond alignment handling, SDL splits requests only where
necessary to represent their byte count in the native signed result.

`SDL_ReadAsyncIO()`, `SDL_WriteAsyncIO()`, and `SDL_LoadFileAsync()` use a separate
positioned native async backend. They use the same alignment and transfer sizing
rules, with one SDL completion per request even when bounce batches need
separate native operations. Async requests do not use the stream region cache.
The caller's buffer must remain valid until the request completes.

`SDL_CopyFile()` uses an aligned 64 KiB transfer buffer. Storage copy operations
use this path as well.

The existing `SDL_HINT_WIIU_FILE_REGION_CACHE_BUFFERS` and
`SDL_HINT_WIIU_FILE_REGION_CACHE_SIZE` hints adjust the stream read cache. Set
them before opening the first file. The defaults allocate 256 KiB for read
regions plus 64 KiB for buffered writes per stream; alignment scratch space is
allocated only when needed, sized for the transfer up to the 1 MiB limit.

# Text input

Use `SDL_StartTextInputWithProperties()` for a focused text field and
`SDL_StopTextInput()` when it loses focus. Starting video does not open a
keyboard. SDL's `SDL_HINT_ENABLE_SCREEN_KEYBOARD` auto policy opens the native
keyboard only when no physical keyboard is attached; physical keyboard text
works independently. The native keyboard runs modally inside the start call,
after SDL has activated text input. Starting text input again can reopen a
submitted or cancelled keyboard, including when text input is still active.

Native OK queues the complete UTF-8 string as `SDL_EVENT_TEXT_INPUT`, before
`SDL_EVENT_SCREEN_KEYBOARD_HIDDEN`. There is no 32-byte limit in SDL3. Cancel
queues no text. Text input remains active until the application stops it.
`SDL_PROP_TEXTINPUT_DEFAULT_TEXT_STRING` supplies prefill; the submitted string
includes that prefill after editing. This is a full-field commit: applications
prefilling an existing field should select its existing text for replacement
before inserting the event. No synthetic Enter/Escape key events are generated.
An empty submission produces no text event. SHOWN/HIDDEN events bracket the
modal; hidden alone does not distinguish cancel from an empty submission.

Type, password visibility, autocorrect, multiline, title/placeholder and maximum
length properties configure the native keyboard. Native maximum lengths count
UTF-16 code units, up to 9999. Text input area coordinates remain available
through `SDL_GetTextInputArea()`; the native full-screen input form has a fixed
layout and does not reposition itself around that rectangle. Controller/touch
presses opening and dismissing the modal are consumed through release. A
foreground generation change aborts the modal without committing; reactivate
the field after foreground reacquisition to reopen it.

With `SDL_HINT_VIDEO_EXTERNAL_GRAPHICS=1`, SDL owns neither GX2 nor scan buffers.
Register `SDL_WiiUTextInputGraphics` from `<SDL3/SDL_wiiu.h>` as
`SDL_PROP_WINDOW_WIIU_TEXT_INPUT_GRAPHICS_POINTER` on the window's properties.
The bridge synchronously dispatches native initialization, updates and shutdown
to the existing GX2 submission thread. Its draw callback supplies distinct
single-sample TV/DRC render targets and presents through existing scan buffers;
it restores renderer context after native display lists. SHUTDOWN executes
cleanup even while foreground storage is unavailable. SDL never initializes or
shuts down a GX2 session in this mode. See the public header for callback rules.
For lousyphreak/bgfx-wiiu, include `<bgfx/platform_gx2_sdl.h>` and call
`bgfx::gx2SetSDLTextInputWindow(window)` once after initializing bgfx. Keep the
existing `bgfx::gx2Suspend()` lifecycle handling and clear the property before
shutting down bgfx. No custom text-field implementation is needed.

The backend's screen-keyboard hook logs failures with `SDL_GetError()` details;
`SDL_StartTextInputWithProperties()` reports activation success independently
of whether SDL's optional screen-keyboard hook can display the native UI.

# Wii Remote mouse

`SDL_SetWindowRelativeMouseMode()` derives relative motion from consecutive
valid IR samples, preserving absolute pointing and buttons outside relative
mode. Acquisition, tracking loss, controller switches, window/size changes,
mode changes and foreground transitions reset the baseline. IR has a finite
tracking area; motion outside it cannot be synthesized. The first sample after
reacquisition establishes position without a relative jump.

# Builds without SDL_Renderer

`SDL_RENDER=OFF` supports video, native text UI, input and external graphics.
The Wii U video backend does not reference optional SDL renderer cleanup in
this configuration.
