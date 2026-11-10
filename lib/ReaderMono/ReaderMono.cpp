#include "ReaderMono.h"
namespace {
const uint8_t regular[] = {
#include "Regular.inc"
};
const uint8_t bold[] = {
#include "Bold.inc"
};
const uint8_t italic[] = {
#include "Italic.inc"
};
const uint8_t boldItalic[] = {
#include "BoldItalic.inc"
};
}  // namespace
const uint8_t* readerMono::data(uint8_t style, size_t* bytes) {
  switch (style & 3) {
    case 1:
      *bytes = sizeof(bold);
      return bold;
    case 2:
      *bytes = sizeof(italic);
      return italic;
    case 3:
      *bytes = sizeof(boldItalic);
      return boldItalic;
    default:
      *bytes = sizeof(regular);
      return regular;
  }
}
