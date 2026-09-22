/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// GXCrcStream.cpp ///////////////////////////////////////////////////////////
// See GXCrcStream.h for what this is for. The arithmetic, in one place:
//
// The checksum consumes one 32-bit word at a time, and the step is invertible:
//
//     forward:  C[i] = ROL(C[i-1], 1) + W[i-1]      (mod 2^32)
//     inverse:  C[i-1] = ROR(C[i] - W[i-1], 1)
//
// Run the inverse from the OTHER machine's final value, using our words, and
// call the result D. Now ask, for each position i: what word would we have had
// to feed at i for our stream to end where theirs did? Everything after i is
// fixed, so there is exactly one such value, and it is
//
//     implied[i] = D[i+1] - ROL(C[i-1], 1)
//
// At the position where the two machines really differ, implied[i] IS their
// word. At every other position it is an arbitrary 32-bit number. So the
// position is found by asking which implied word looks like a plausible
// perturbation of ours -- same sign, same exponent, a handful of mantissa bits
// apart, which is what platform rounding produces and what a random number
// almost never is.
//
// Measured on synthetic streams of twenty thousand words in the same value
// ranges the simulation uses: a single one-ULP difference leaves about
// twenty-five surviving candidates, and the true one ranks first or third.
// With the section labels recorded alongside, that is an object and a field.

#include "PreRTS.h"

#include "Common/GXCrcStream.h"
#include "GXTrace.h"
#include "Utility/endian_compat.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{
	// A Zero Hour map with three hundred objects produces tens of thousands of
	// words, so a few hundred KB. The cap exists only so that a pathological map
	// cannot exhaust a phone's memory; being told the cap was hit beats being
	// handed a silently truncated answer.
	const size_t MAX_WORDS = 1024u * 1024u;

	// Why a ring and not one buffer: in a live network game the other machine's
	// checksum for frame N does not arrive until a few frames later, by which time
	// a single buffer would already hold a different frame's words and the locator
	// would have to decline. Keeping the last few snapshots costs a megabyte or so
	// at real map sizes and makes the tool work in a match, not only in a replay.
	const size_t RING = 4;

	// How many candidates to print. The true position has ranked first or third
	// in every synthetic test; a dozen is room to spare without burying the log.
	const size_t MAX_REPORTED = 12;

	struct Mark
	{
		size_t pos;
		std::string label;
	};

	// What kind of difference a position would have to be. Ordered by how
	// distinctive it is: a random 32-bit number has a 32-in-2^32 chance of sitting
	// one bit away from ours, so over eighty thousand positions a bit-flip hit is
	// essentially never a coincidence. Float rounding is the loosest of the four
	// and so comes last.
	enum CandidateKind
	{
		KIND_BIT_FLIP = 0,       // a status bit, a flag, a mask
		KIND_SMALL_INT,          // a counter, a frame number, an object id
		KIND_ZERO_ONE_SIDE,      // a field set on one machine and not the other
		KIND_FLOAT_ROUNDING,     // platform arithmetic
		KIND_COUNT
	};

	const char *kindName( Int kind )
	{
		switch (kind)
		{
			case KIND_BIT_FLIP:      return "up to four bits apart (a flag or status bit)";
			case KIND_SMALL_INT:     return "a small integer apart (a counter, frame or id)";
			case KIND_ZERO_ONE_SIDE: return "zero on one machine only";
			case KIND_FLOAT_ROUNDING:return "float rounding";
			default:                 return "?";
		}
	}

	struct Candidate
	{
		size_t index;
		UnsignedInt impliedWord;
		Int kind;
		Int distance;            // bits, or integer delta, or mantissa delta
	};

	struct CrcSnapshot
	{
		std::vector<UnsignedInt> words;   // the operand fed in at each step
		std::vector<UnsignedInt> crcs;    // our running value after each step
		std::vector<Mark> marks;
		UnsignedInt frame = 0;
		Bool truncated = false;
		Bool complete = false;
	};

	CrcSnapshot theRing[RING];
	size_t theCurrent = 0;
	Bool theCapturing = false;

	inline CrcSnapshot &cur() { return theRing[theCurrent]; }

	inline UnsignedInt rol1( UnsignedInt x )
	{
		return (x << 1) | ((x >> 31) & 0x01u);
	}

	inline UnsignedInt ror1( UnsignedInt x )
	{
		return (x >> 1) | ((x & 0x01u) << 31);
	}

	// The words go in byte-swapped (XferCRC::addCRC applies htobe), so undo that
	// to get the field's own bytes back before reading them as a number.
	inline UnsignedInt fieldBits( UnsignedInt wordAsFed )
	{
		return htobe(wordAsFed);
	}

	inline float asFloat( UnsignedInt bits )
	{
		float f;
		memcpy(&f, &bits, sizeof(f));
		return f;
	}

	const char *labelForPosition( const CrcSnapshot &snap, size_t pos, size_t *offsetWithin )
	{
		const Mark *best = nullptr;
		for (size_t i = 0; i < snap.marks.size(); ++i)
		{
			if (snap.marks[i].pos <= pos && (best == nullptr || snap.marks[i].pos >= best->pos))
				best = &snap.marks[i];
		}
		if (best == nullptr)
		{
			*offsetWithin = pos;
			return "(before the first mark)";
		}
		*offsetWithin = pos - best->pos;
		return best->label.c_str();
	}

	// Does 'implied' look like our word with platform rounding applied to it?
	// Same sign, same exponent, an exponent in the range the simulation actually
	// uses, and only low mantissa bits apart.
	Bool looksLikeRounding( UnsignedInt ourBits, UnsignedInt theirBits, Int *mantissaDelta )
	{
		if (ourBits == theirBits)
			return false;

		if ((ourBits >> 31) != (theirBits >> 31))
			return false;

		const UnsignedInt ourExp = (ourBits >> 23) & 0xFFu;
		const UnsignedInt theirExp = (theirBits >> 23) & 0xFFu;
		if (ourExp != theirExp)
			return false;

		// Exponent 100..150 is roughly 1e-8 to 1e11: coordinates, angles, health,
		// velocities. Below that the word is almost certainly a small integer being
		// read as a denormal, which matches by accident far too often.
		if (ourExp < 100u || ourExp > 150u)
			return false;

		const Int ourMantissa = (Int)(ourBits & 0x7FFFFFu);
		const Int theirMantissa = (Int)(theirBits & 0x7FFFFFu);
		const Int delta = ourMantissa > theirMantissa
			? ourMantissa - theirMantissa : theirMantissa - ourMantissa;
		if (delta > 64)
			return false;

		*mantissaDelta = delta;
		return true;
	}

	inline Int popcount32( UnsignedInt x )
	{
		Int n = 0;
		while (x) { x &= (x - 1); ++n; }
		return n;
	}

	// Classify the one word that would reconcile our stream with theirs. Returns
	// false when it looks like nothing in particular, which is what an arbitrary
	// 32-bit number does and therefore what almost every position does.
	Bool classify( UnsignedInt ourBits, UnsignedInt theirBits, Int *kind, Int *distance )
	{
		if (ourBits == theirBits)
			return false;

		const Int bits = popcount32(ourBits ^ theirBits);
		if (bits <= 4)
		{
			*kind = KIND_BIT_FLIP;
			*distance = bits;
			return true;
		}

		// GeneralsX @bugfix Android port 21/09/2026 Only the direction that is rare.
		//
		// This was `ourBits == 0 || theirBits == 0`, and the checksum stream is full
		// of zero words -- unset fields, padding, empty shroud. Wherever ours was a
		// zero, ANY implied value satisfied the test, so on the first real log the
		// category claimed 7665 of 85538 positions and drowned the report. Ours
		// non-zero against an implied zero is the informative direction and is as
		// rare as any other single value: a field this machine set and the other
		// did not.
		if (theirBits == 0u && ourBits != 0u)
		{
			*kind = KIND_ZERO_ONE_SIDE;
			*distance = 0;
			return true;
		}

		const UnsignedInt diff = (ourBits > theirBits)
			? (ourBits - theirBits) : (theirBits - ourBits);
		if (diff <= 4096u)
		{
			*kind = KIND_SMALL_INT;
			*distance = (Int)diff;
			return true;
		}

		Int mantissaDelta = 0;
		if (looksLikeRounding(ourBits, theirBits, &mantissaDelta))
		{
			*kind = KIND_FLOAT_ROUNDING;
			*distance = mantissaDelta;
			return true;
		}

		return false;
	}

	Bool byDistinctiveness( const Candidate &a, const Candidate &b )
	{
		if (a.kind != b.kind)
			return a.kind < b.kind;
		if (a.distance != b.distance)
			return a.distance < b.distance;
		return a.index < b.index;
	}
}

namespace GXCrcStream
{

Bool isCapturing()
{
	return theCapturing;
}

void begin( UnsignedInt frame )
{
	if (!GXTrace::isNetEnabled())
	{
		theCapturing = false;
		return;
	}

	theCurrent = (theCurrent + 1) % RING;
	CrcSnapshot &snap = cur();
	snap.words.clear();
	snap.crcs.clear();
	snap.marks.clear();
	snap.truncated = false;
	snap.complete = false;
	snap.frame = frame;
	theCapturing = true;
}

void end()
{
	if (theCapturing)
		cur().complete = true;
	theCapturing = false;
}

void push( UnsignedInt word, UnsignedInt crcAfter )
{
	if (!theCapturing)
		return;

	CrcSnapshot &snap = cur();
	if (snap.words.size() >= MAX_WORDS)
	{
		snap.truncated = true;
		return;
	}

	snap.words.push_back(word);
	snap.crcs.push_back(crcAfter);
}

void mark( const char *label )
{
	if (!theCapturing || label == nullptr)
		return;

	CrcSnapshot &snap = cur();
	Mark m;
	m.pos = snap.words.size();
	m.label = label;
	snap.marks.push_back(m);
}

void markObject( UnsignedInt objectId, const char *templateName )
{
	if (!theCapturing)
		return;

	char buf[128];
	snprintf(buf, sizeof(buf), "object id=%u %s",
		(unsigned)objectId, templateName ? templateName : "(no template)");
	mark(buf);
}

namespace
{
	Bool haveStreamEndingIn( UnsignedInt crcAsReported )
	{
		const UnsignedInt internalValue = htobe(crcAsReported);
		for (size_t k = 0; k < RING; ++k)
		{
			if (!theRing[k].crcs.empty() && theRing[k].crcs.back() == internalValue)
				return true;
		}
		return false;
	}
}

void dumpSection( const char *sectionLabel, UnsignedInt theirCRC, UnsignedInt ourCRC )
{
	if (!GXTrace::isNetEnabled() || sectionLabel == nullptr)
		return;

	// GeneralsX @tweak Android port 22/09/2026 Three checkpoints, not one.
	//
	// One dump settles every hypothesis about a single word, because the implied
	// word at the true position is the same at every checkpoint while everywhere
	// else it moves -- that filter cut twelve candidates to three. A hypothesis
	// about two words has one equation and two unknowns, so a single dump admits
	// a solution almost everywhere and the surviving candidates are numerology:
	// 23 came back on the last sweep, all of them requiring a structurally-zero
	// word to become 0xFFFFFFFC or a byte inside a weapon's name to change.
	//
	// Three dumps make that filter available to multi-word hypotheses too: a real
	// static difference holds at the same positions with the same deltas on every
	// checkpoint, and three independent 32-bit constraints are more than a pair of
	// positions and deltas has freedom to fake. Each dump is about 770 KB of log
	// against a session that already produces megabytes.
	static Int dumpsLeft = -1;
	if (dumpsLeft < 0)
	{
		const char *env = getenv("GX_CRC_DUMPS");
		dumpsLeft = (env != nullptr && *env != '\0') ? atoi(env) : 3;
		if (dumpsLeft < 0)
			dumpsLeft = 0;
	}
	// A checkpoint whose object list differs from every dumped one is worth a dump
	// of its own, whatever the count says: a different list means different running
	// values through the objects section, which is a new equation rather than the
	// same one measured again. Two of those on top of the regular three.
	static size_t lastDumpedWords = 0;
	static Int listChangeDumpsLeft = 2;
	Bool listChanged = FALSE;
	if (dumpsLeft == 0)
	{
		const UnsignedInt ourNow = htobe(ourCRC);
		for (size_t k = 0; k < RING; ++k)
		{
			if (!theRing[k].crcs.empty() && theRing[k].crcs.back() == ourNow)
			{
				listChanged = lastDumpedWords != 0 && theRing[k].words.size() != lastDumpedWords;
				break;
			}
		}
		if (!listChanged || listChangeDumpsLeft == 0)
			return;
	}

	const UnsignedInt ourInternal = htobe(ourCRC);
	const CrcSnapshot *found = nullptr;
	for (size_t k = 0; k < RING; ++k)
	{
		if (!theRing[k].crcs.empty() && theRing[k].crcs.back() == ourInternal)
		{
			found = &theRing[k];
			break;
		}
	}
	if (found == nullptr)
		return;

	const CrcSnapshot &snap = *found;
	const size_t n = snap.words.size();

	// GeneralsX @tweak Android port 22/09/2026 "*" dumps the whole stream.
	//
	// Dumping one section assumes the difference is inside it, and the evidence
	// for that assumption turned out to be worth less than it looked: the inverse
	// walk rotates a difference rather than shrinking it, so a small difference at
	// the section's end is equally consistent with one small word difference
	// anywhere in the eighty thousand words after it. Every word costs nine bytes
	// of log, the whole stream is under a megabyte, and having all of it removes
	// the assumption instead of arguing about it.
	const Bool whole = (strcmp(sectionLabel, "*") == 0);

	// Locate the named section: from its mark to the next named one.
	size_t from = whole ? 0 : n, to = n;
	for (size_t k = 0; !whole && k < snap.marks.size(); ++k)
	{
		if (snap.marks[k].label != sectionLabel)
			continue;
		from = snap.marks[k].pos;
		to = n;
		for (size_t j = k + 1; j < snap.marks.size(); ++j)
		{
			if (snap.marks[j].label.compare(0, 7, "object ") != 0
				&& snap.marks[j].label.compare(0, 6, "cells ") != 0)
			{
				to = snap.marks[j].pos;
				break;
			}
		}
		break;
	}
	if (from >= to)
		return;

	// The inverse walk of their checksum back to the section's end. Everything
	// inside the section is then reconstructable from this and our words.
	UnsignedInt theirs = htobe(theirCRC);
	for (size_t i = n; i > to; --i)
		theirs = ror1(theirs - snap.words[i - 1]);

	const UnsignedInt ourAtFrom = (from >= 1) ? snap.crcs[from - 1] : 0u;
	const UnsignedInt ourAtTo = (to >= 1) ? snap.crcs[to - 1] : 0u;

	GX_NET_TRACE("crc dump frame %u: section '%s' words %u..%u count=%u\n",
		(unsigned)snap.frame, sectionLabel, (unsigned)from, (unsigned)to,
		(unsigned)(to - from));
	GX_NET_TRACE("crc dump frame %u: ourAtStart=%08X ourAtEnd=%08X theirAtEnd=%08X"
		" (accumulator values, not byte-swapped)\n",
		(unsigned)snap.frame, (unsigned)ourAtFrom, (unsigned)ourAtTo, (unsigned)theirs);

	// The object and cell marks inside the section, so an offset can be named.
	for (size_t k = 0; k < snap.marks.size(); ++k)
	{
		if (snap.marks[k].pos < from || snap.marks[k].pos >= to)
			continue;
		if (!whole && snap.marks[k].label.compare(0, 7, "object ") != 0)
			continue;
		GX_NET_TRACE("crc dump frame %u: mark %u %s\n", (unsigned)snap.frame,
			(unsigned)(snap.marks[k].pos - from), snap.marks[k].label.c_str());
	}

	// The words themselves, as fed to the accumulator, sixteen to a line.
	char line[16 * 9 + 8];
	size_t col = 0;
	size_t len = 0;
	for (size_t i = from; i < to; ++i)
	{
		len += snprintf(line + len, sizeof(line) - len, "%08X ", (unsigned)snap.words[i]);
		if (++col == 16 || i + 1 == to)
		{
			GX_NET_TRACE("crc dump frame %u: w %u %s\n", (unsigned)snap.frame,
				(unsigned)(i + 1 - col - from), line);
			col = 0;
			len = 0;
		}
	}

	lastDumpedWords = n;
	if (listChanged)
		--listChangeDumpsLeft;
	else
		--dumpsLeft;
}

void reportEither( UnsignedInt crcA, UnsignedInt crcB )
{
	if (!GXTrace::isNetEnabled())
		return;

	if (haveStreamEndingIn(crcA))
		report(crcB, crcA);
	else if (haveStreamEndingIn(crcB))
		report(crcA, crcB);
	else
		GX_NET_TRACE("crc locate: neither %08X nor %08X ends a captured stream, so"
			" the compared checksums were generated outside the last %u captures.\n",
			(unsigned)crcA, (unsigned)crcB, (unsigned)RING);
}

// GeneralsX @feature Android port 22/09/2026 Test a hypothesis at every checkpoint.
//
// Two PC recordings of the same map pin the lockstep difference to the objects
// section, and within it one family of explanation reproduces both recordings
// exactly: both AmericaCheckpoint objects carrying m22 = 0.99999994 on the PC
// where we have 1.0 -- a one-ULP difference in a computed rotation element, the
// signature of x87 against ARM float. Two recordings are not a proof, though: an
// aliased variant (a zero word two units low, sixteen words further on) fits them
// just as well, because in that stretch of the stream both recordings impose
// nearly the same equation.
//
// What separates a real explanation from its alias is a checkpoint whose object
// list is different -- a building placed, a unit created -- because that changes
// every running value the difference passes through. So instead of asking for
// more dumps, the engine applies each hypothesis to its own captured words at
// every mismatching checkpoint and says whether the result is the PC's number.
// The real one matches everywhere; an alias falls apart the first time the list
// changes. Each test is one pass over the captured words, a few per checkpoint.
namespace
{
	struct GXCrcHypothesis
	{
		const char *description;
		const char *templateName;   // nullptr: any template
		size_t      offset;         // word offset from the object's mark
		Int         fieldDelta;     // added to the field value (after the byte swap)
		Bool        onlyQuarterTurns; // only objects whose m00 is float cos(90 deg)
	};

	const GXCrcHypothesis kHypotheses[] =
	{
		{ "AmericaCheckpoint m22 -1 ulp",         "AmericaCheckpoint", 11, -1, FALSE },
		{ "AmericaCheckpoint +27 -2 (the alias)", "AmericaCheckpoint", 27, -2, FALSE },
		{ "every quarter-turned object m22 -1 ulp", nullptr,           11, -1, TRUE  },
	};

	Bool markIsObjectOf( const std::string &label, const char *templateName )
	{
		if (label.compare(0, 10, "object id=") != 0)
			return FALSE;
		if (templateName == nullptr)
			return TRUE;
		const size_t space = label.find(' ', 10);
		return space != std::string::npos && label.compare(space + 1, std::string::npos, templateName) == 0;
	}

	void testHypotheses( const CrcSnapshot &snap, UnsignedInt theirInternal )
	{
		const size_t n = snap.words.size();
		const UnsignedInt kQuarterTurnCos = 0xB33BBD2Eu; // (float)cos(PI/2)
		for (size_t h = 0; h < sizeof(kHypotheses) / sizeof(kHypotheses[0]); ++h)
		{
			const GXCrcHypothesis &hyp = kHypotheses[h];
			std::vector<size_t> patch;
			for (size_t k = 0; k < snap.marks.size(); ++k)
			{
				const size_t at = snap.marks[k].pos + hyp.offset;
				if (!markIsObjectOf(snap.marks[k].label, hyp.templateName) || at >= n)
					continue;
				if (hyp.onlyQuarterTurns && fieldBits(snap.words[snap.marks[k].pos + 1]) != kQuarterTurnCos)
					continue;
				patch.push_back(at);
			}
			std::sort(patch.begin(), patch.end());

			UnsignedInt crc = 0;
			size_t next = 0;
			for (size_t i = 0; i < n; ++i)
			{
				UnsignedInt word = snap.words[i];
				if (next < patch.size() && patch[next] == i)
				{
					word = htobe(htobe(word) + (UnsignedInt)hyp.fieldDelta);
					++next;
				}
				crc = rol1(crc) + word;
			}

			GX_NET_TRACE("crc hyp frame %u: %-40s patched %2u words over %u -> %s"
				" (%08X, theirs %08X)\n",
				(unsigned)snap.frame, hyp.description, (unsigned)patch.size(), (unsigned)n,
				crc == theirInternal ? "MATCHES THE PC" : "no", (unsigned)crc, (unsigned)theirInternal);
		}
	}
}

void report( UnsignedInt theirCRC, UnsignedInt ourCRC )
{
	if (!GXTrace::isNetEnabled())
		return;

	// getCRC() byte-swaps on the way out; the walk works on the accumulator's own
	// value, so undo it.
	const UnsignedInt theirInternal = htobe(theirCRC);
	const UnsignedInt ourInternal = htobe(ourCRC);

	// Find the snapshot that produced the value being compared. In a replay that
	// is almost always the newest one; in a live match the peer's checksum for
	// frame N arrives a few frames later, which is the reason for the ring.
	const CrcSnapshot *found = nullptr;
	for (size_t k = 0; k < RING; ++k)
	{
		const CrcSnapshot &snap = theRing[k];
		if (!snap.crcs.empty() && snap.crcs.back() == ourInternal)
		{
			found = &snap;
			break;
		}
	}

	if (found == nullptr)
	{
		GX_NET_TRACE("crc locate: no captured stream ends at %08X, so the checksum"
			" being compared was generated outside the last %u captures. Nothing to"
			" locate against.\n", (unsigned)ourCRC, (unsigned)RING);
		return;
	}

	const CrcSnapshot &snap = *found;
	const size_t n = snap.words.size();

	if (snap.truncated)
	{
		GX_NET_TRACE("crc locate frame %u: capture hit its %u-word cap, so this"
			" covers only the first part of the checksum\n",
			(unsigned)snap.frame, (unsigned)MAX_WORDS);
	}

	// Inverse walk from their final value, using our words.
	std::vector<UnsignedInt> back(n + 1, 0);
	back[n] = theirInternal;
	for (size_t i = n; i > 0; --i)
		back[i - 1] = ror1(back[i] - snap.words[i - 1]);

	std::vector<Candidate> cands;
	for (size_t i = 0; i < n; ++i)
	{
		const UnsignedInt ourPrev = (i >= 1) ? snap.crcs[i - 1] : 0u;
		const UnsignedInt implied = back[i + 1] - rol1(ourPrev);

		Int kind = 0, distance = 0;
		if (classify(fieldBits(snap.words[i]), fieldBits(implied), &kind, &distance))
		{
			Candidate c;
			c.index = i;
			c.impliedWord = implied;
			c.kind = kind;
			c.distance = distance;
			cands.push_back(c);
		}
	}

	Int perKind[KIND_COUNT] = { 0 };
	for (size_t k = 0; k < cands.size(); ++k)
		++perKind[cands[k].kind];

	GX_NET_TRACE("crc locate frame %u: ours=%08X theirs=%08X over %u words;"
		" %u positions could be a single-word difference"
		" (%d bit-flip, %d small-int, %d zero-one-side, %d float-rounding)\n",
		(unsigned)snap.frame, (unsigned)ourCRC, (unsigned)theirCRC,
		(unsigned)n, (unsigned)cands.size(),
		(int)perKind[KIND_BIT_FLIP], (int)perKind[KIND_SMALL_INT],
		(int)perKind[KIND_ZERO_ONE_SIDE], (int)perKind[KIND_FLOAT_ROUNDING]);

	// The section layout is worth having either way: it says how the words are
	// distributed, which is the first thing to compare against the other machine
	// when the difference turns out to be structural rather than one word.
	for (size_t k = 0; k < snap.marks.size(); ++k)
	{
		// Objects get one mark each and there can be hundreds; the named sections
		// are the useful skeleton.
		if (snap.marks[k].label.compare(0, 7, "object ") == 0
			|| snap.marks[k].label.compare(0, 6, "cells ") == 0)
			continue;
		// GeneralsX @bugfix Android port 21/09/2026 End at the next NAMED section, not
		// at the next mark. The next mark after "Objects" is the first object's, so
		// the objects section reported four words instead of six thousand.
		size_t endPos = n;
		for (size_t j = k + 1; j < snap.marks.size(); ++j)
		{
			if (snap.marks[j].label.compare(0, 7, "object ") != 0
				&& snap.marks[j].label.compare(0, 6, "cells ") != 0)
			{
				endPos = snap.marks[j].pos;
				break;
			}
		}
		GX_NET_TRACE("crc locate frame %u: section %-22s words %u..%u (%u)\n",
			(unsigned)snap.frame, snap.marks[k].label.c_str(),
			(unsigned)snap.marks[k].pos, (unsigned)endPos,
			(unsigned)(endPos - snap.marks[k].pos));
	}

	if (cands.empty())
	{
		GX_NET_TRACE("crc locate frame %u: no single word reconciles the two. So more"
			" than one word differs -- a Coord3D or a transform, a field block, or a"
			" different number of objects. The word count above is what to compare"
			" against the other machine next.\n", (unsigned)snap.frame);
		return;
	}

	std::sort(cands.begin(), cands.end(), byDistinctiveness);

	size_t lo = cands.front().index;
	size_t hi = cands.front().index;
	for (size_t k = 0; k < cands.size(); ++k)
	{
		lo = std::min(lo, cands[k].index);
		hi = std::max(hi, cands[k].index);
	}
	size_t offLo = 0, offHi = 0;
	const char *labelLo = labelForPosition(snap, lo, &offLo);
	const char *labelHi = labelForPosition(snap, hi, &offHi);
	GX_NET_TRACE("crc locate frame %u: they all fall between word %u (%s +%u) and"
		" word %u (%s +%u)\n",
		(unsigned)snap.frame, (unsigned)lo, labelLo, (unsigned)offLo,
		(unsigned)hi, labelHi, (unsigned)offHi);

	const size_t show = std::min(cands.size(), MAX_REPORTED);
	for (size_t k = 0; k < show; ++k)
	{
		const Candidate &c = cands[k];
		const UnsignedInt ourBits = fieldBits(snap.words[c.index]);
		const UnsignedInt theirBits = fieldBits(c.impliedWord);
		size_t offsetWithin = 0;
		const char *label = labelForPosition(snap, c.index, &offsetWithin);
		GX_NET_TRACE("crc locate frame %u:   #%u word %u  %s +%u  ours=%08X (%d, %.9g)"
			"  theirs=%08X (%d, %.9g)  %s, distance %d\n",
			(unsigned)snap.frame, (unsigned)(k + 1), (unsigned)c.index,
			label, (unsigned)offsetWithin,
			(unsigned)ourBits, (int)ourBits, asFloat(ourBits),
			(unsigned)theirBits, (int)theirBits, asFloat(theirBits),
			kindName(c.kind), (int)c.distance);
	}

	testHypotheses(snap, theirInternal);
}

} // namespace GXCrcStream
