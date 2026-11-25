#include "test.h"
#include "width.c"

int main(void) {
	CHECK(width_of('a') == 1 && width_of(0xF1) == 1, "Latin is one cell");
	CHECK(width_of(0x0301) == 0, "a combining acute takes none");
	CHECK(width_of(0x65E5) == 2, "a CJK ideograph takes two");
	CHECK(width_of(0x1F642) == 2, "an emoji takes two");
	CHECK(width_of(0x2713) == 1, "a check mark takes one");
	CHECK(width_of(0x200B) == 0 && width_of(0x200D) == 0,
	      "zero width space and joiner take none");
	CHECK(width_of(0x1160) == 0, "a Hangul medial vowel takes none");
	CHECK(width_of(0xAC00) == 2, "a Hangul syllable takes two");
	CHECK(width_of(0x00AD) == 1, "the soft hyphen takes one");
	return test_summary();
}
