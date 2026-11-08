#include <gtest/gtest.h>

#include "BufferedGrayscalePlanes.h"
TEST(BufferedGrayscaleNoPsram, KeepsExistingPathWithoutAllocation) {
  BufferedGrayscalePlanes p;
  EXPECT_FALSE(p.ensure(48000));
  EXPECT_EQ(p.lsb(), nullptr);
  EXPECT_EQ(p.msb(), nullptr);
  p.reset();
}
