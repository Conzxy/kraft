#include "protobuf_test.pb.h"

#include <iostream>

int main()
{
  TestMessage message;
  message.set_str("xx");
  std::cout << message.DebugString();
}
