// log.h - tiny file logger (writes jpogmod.log next to the game exe).
#pragma once
namespace modlog {
    void Init(bool enabled);
    void Line(const char* fmt, ...);
    void Shutdown();
}
#define LOG(...) ::modlog::Line(__VA_ARGS__)
