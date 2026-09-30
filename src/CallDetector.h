#pragma once

#include <string>
#include <vector>

// A call that seems to be going on now.
struct DetectedCall {
    std::wstring app;      // CallLogic::CallApp::name, or "Google Meet"
    std::wstring subject;  // the meeting's name from its window title; often empty
};

// Which calling apps are capturing from a microphone right now (Windows'
// audio sessions, as in the Volume Mixer), plus the meeting's name from their
// window titles where the app gives one. Browsers only count while a window
// shows a meeting. Nothing is stored or sent anywhere. Takes a few
// milliseconds. Needs COM initialised on the calling thread.
std::vector<DetectedCall> DetectCalls();
