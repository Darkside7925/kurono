#ifndef KURONO_AUDIO_PULSE_SERVER_H
#define KURONO_AUDIO_PULSE_SERVER_H

#include "../kernel/types.h"

// PulseAudio-compatible socket server.
//
// Listens at /system/run/user/1000/pulse/native and speaks just enough
// of the PulseAudio native protocol for libpulse + libpulse-simple
// clients (Firefox, GTK apps) to connect, enumerate sinks/sources, and
// stream audio frames.
//
// Wire protocol header (20 bytes per packet):
//   uint32  length          (payload length)
//   uint32  channel         (0xFFFFFFFF = command channel)
//   uint64  offset_hi:lo    (stream byte offset)
//   uint32  flags
//
// Followed by tagstruct-encoded payload.  We respond to:
//
//   PA_COMMAND_AUTH                 -> reply with version+cookie ack
//   PA_COMMAND_SET_CLIENT_NAME      -> reply with client index
//   PA_COMMAND_GET_SERVER_INFO      -> name="kurono-pulse" + default sink
//   PA_COMMAND_GET_SINK_INFO_LIST   -> one sink ("alsa_output.kurono")
//   PA_COMMAND_GET_SOURCE_INFO_LIST -> one source
//   PA_COMMAND_CREATE_PLAYBACK_STREAM -> stream channel + buffer attrs
//   PA_COMMAND_DRAIN_PLAYBACK_STREAM  -> ack
//   PA_COMMAND_DELETE_PLAYBACK_STREAM -> ack
//
// All other commands return an empty success reply so the client keeps
// running.  Audio frames received on a stream channel are forwarded to
// the existing HDA/AC97/SB16 driver via Audio::PlayPCM.

namespace PulseServer {

    static const int PA_MAX_CLIENTS = 16;
    static const int PA_MAX_STREAMS_PER_CLIENT = 8;

    void Init();

    int  ListenSd();
    int  ClientCount();
    int  StreamCount();
}

#endif
