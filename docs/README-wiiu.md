# Wii U file I/O

`SDL_IOFromFile()` uses native Cafe FS for both loose SD files and application
content. `SDL_ReadStorageFile()`, `SDL_WriteStorageFile()`, `SDL_LoadFile()`, and
`SDL_SaveFile()` use the same stream backend.

Small reads fill a 64 KiB region cache. Each stream keeps four regions by default
and prefetches the next region while the application consumes cached bytes. A
read that catches up with prefetch waits for that operation and uses its result.
Bulk reads consume any relevant cached/prefetched bytes, then transfer directly
to the caller's buffer without filling successive cache regions.

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
