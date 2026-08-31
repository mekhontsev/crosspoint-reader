#pragma once

#include <string>

#include "activities/Activity.h"

class PluginErrorActivity final : public Activity {
 public:
  PluginErrorActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const char* message);

  void loop() override;
  void render(RenderLock&&) override;
  bool handleHomeGesture() override;

 private:
  std::string message_;
};
