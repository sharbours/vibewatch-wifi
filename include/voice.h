#pragma once

// Push-to-talk capture from the StopWatch MEMS mic and playback of the spoken
// reply. The mic and speaker share the ES8311's I2S clock pins, so only one
// can be active at a time: capture ends the speaker, and stopping capture
// restores it.

#include <cstddef>
#include <cstdint>

namespace voice {

void begin();

// Called from the main loop every iteration.
void loop();

// Push-to-talk edges. `agentSlot` (0-5) tells the bridge which Hermes
// conversation the utterance belongs to.
void pressToTalk(int agentSlot, int profile);
void releaseToTalk();

bool isCapturing();
bool isSpeaking();
void setVolume(std::uint8_t volume);

// Transport callbacks (called from net_link on the main loop task).
void onControl(const char* json);  // {"m":"tts.begin"|"tts.end"|"tts.stop",...}
void onAudioChunk(const std::uint8_t* data, std::size_t length);
void onLinkLost();

}  // namespace voice
