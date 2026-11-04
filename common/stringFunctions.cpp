#include "stringFunctions.hpp"

// Standard C++ library header files.
#include <algorithm>
#include <limits>

void appendNumber(char *dest, std::size_t destSize, std::size_t index)
{
  if (destSize == 0)
  {
    return;
  }

  char digits[std::numeric_limits<std::size_t>::digits10 + 1];
  std::size_t count = 0;
  do
  {
    digits[count++] = '0' + (index % 10);
    index /= 10;
  } while (index != 0);

  count = std::min(destSize-1, count);

  while (count != 0)
  {
    *dest++ = digits[--count];
  }

  *dest = '\0';
}