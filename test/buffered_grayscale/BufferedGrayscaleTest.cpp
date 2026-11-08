#include <gtest/gtest.h>

#include "BufferedGrayscalePlanes.h"
TEST(BufferedGrayscale, TwoPlanesReuseAndRelease) {
  fakepsram::reset();
  {
    BufferedGrayscalePlanes p;
    ASSERT_TRUE(p.ensure(48000));
    EXPECT_EQ(p.msb() - p.lsb(), 48000);
    EXPECT_EQ(fakepsram::lastCaps, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    auto* first = p.lsb();
    ASSERT_TRUE(p.ensure(48000));
    EXPECT_EQ(first, p.lsb());
    EXPECT_EQ(fakepsram::attempts, 1);
    ASSERT_TRUE(p.ensure(64800));
    EXPECT_EQ(p.msb() - p.lsb(), 64800);
    EXPECT_EQ(fakepsram::allocated, 1);
    p.reset();
    EXPECT_EQ(p.lsb(), nullptr);
    EXPECT_EQ(p.msb(), nullptr);
  }
  EXPECT_EQ(fakepsram::allocated, 0);
}
TEST(BufferedGrayscale, LowOrFragmentedPsramNeverFallsBackToInternalMemory) {
  fakepsram::reset();
  BufferedGrayscalePlanes p;
  fakepsram::freeBytes = 96000 + 128 * 1024 - 1;
  EXPECT_FALSE(p.ensure(48000));
  EXPECT_EQ(fakepsram::attempts, 0);
  fakepsram::freeBytes = 8 * 1024 * 1024;
  fakepsram::largest = 95999;
  EXPECT_FALSE(p.ensure(48000));
  EXPECT_EQ(fakepsram::attempts, 0);
  fakepsram::largest = 96000;
  fakepsram::fail = true;
  EXPECT_FALSE(p.ensure(48000));
  EXPECT_EQ(fakepsram::attempts, 1);
  EXPECT_EQ(p.lsb(), nullptr);
  EXPECT_EQ(p.msb(), nullptr);
  EXPECT_EQ(fakepsram::allocated, 0);
  fakepsram::fail = false;
  EXPECT_TRUE(p.ensure(48000));
}
TEST(BufferedGrayscale, RejectsInvalidSizesAndDestructorFreesStorage) {
  fakepsram::reset();
  {
    BufferedGrayscalePlanes p;
    EXPECT_FALSE(p.ensure(0));
    EXPECT_FALSE(p.ensure(std::numeric_limits<size_t>::max()));
    EXPECT_EQ(fakepsram::attempts, 0);
    EXPECT_TRUE(p.ensure(48000));
  }
  EXPECT_EQ(fakepsram::allocated, 0);
}
