#pragma once
#include <stream/stream.h>
Stream* buffered_file_stream_alloc(Storage* storage);
bool buffered_file_stream_open(Stream* s, const char* path, FS_AccessMode a, FS_OpenMode m);
bool buffered_file_stream_close(Stream* s);
