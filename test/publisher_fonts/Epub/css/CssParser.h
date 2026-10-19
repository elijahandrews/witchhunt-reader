#pragma once
#include <Epub/css/CssFontCatalog.h>
// The manager only queries this accessor. Parsing/catalog storage is production code.
class CssParser {
 public:
  CssFontCatalog catalog;
  const CssFontCatalog& fontCatalog() const { return catalog; }
};
