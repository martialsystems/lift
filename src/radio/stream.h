// Copyright (c) 2026 Martial Systems LLC. All rights reserved.

#pragma once

// Net thread. Pushes codec bytes, takes 48 kHz stereo frames.
struct StreamDecoder;

StreamDecoder* stream_open(const char* codec);
void stream_close(StreamDecoder* decoder);
bool stream_push(StreamDecoder* decoder, const unsigned char* data, int n);
int stream_take(StreamDecoder* decoder, float* stereo, int maxFrames);
