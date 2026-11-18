#pragma once
#include <Arduino.h>

class Console {
 public:
  void begin();
  void task();
  void run(String line);

 private:
  String line_;
};

extern Console console;
