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
#include <cstring>
#include <string>
#include <vector>

namespace
{
	// A Zero Hour map with three hundred objects produces tens of thousands of
	// words, so a few hundred KB. The cap exists only so that a pathological map
	// cannot exhaust a phone's memory; being told the cap was hit beats being
	// handed a silently truncated answer.
	const size_t MAX_WORDS = 4u * 1024u * 1024u;

	// How many candidates to print. The true position has ranked first or third
	// in every synthetic test; a dozen is room to spare without burying the log.
	const size_t MAX_REPORTED = 12;

	struct Mark
	{
		size_t pos;
		std::string label;
	};

	struct Candidate
	{
		size_t index;
		UnsignedInt impliedWord;
		Int mantissaDelta;
	};

	std::vector<UnsignedInt> theWords;   // the operand fed in at each step
	std::vector<UnsignedInt> theCrcs;    // our running value after each step
	std::vector<Mark> theMarks;
	Bool theCapturing = false;
	Bool theTruncated = false;
	UnsignedInt theFrame = 0;

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

	const char *labelForPosition( size_t pos, size_t *offsetWithin )
	{
		const Mark *best = nullptr;
		for (size_t i = 0; i < theMarks.size(); ++i)
		{
			if (theMarks[i].pos <= pos && (best == nullptr || theMarks[i].pos >= best->pos))
				best = &theMarks[i];
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

	Bool byMantissaDelta( const Candidate &a, const Candidate &b )
	{
		if (a.mantissaDelta != b.mantissaDelta)
			return a.mantissaDelta < b.mantissaDelta;
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

	theWords.clear();
	theCrcs.clear();
	theMarks.clear();
	theTruncated = false;
	theFrame = frame;
	theCapturing = true;
}

void end()
{
	theCapturing = false;
}

void push( UnsignedInt word, UnsignedInt crcAfter )
{
	if (!theCapturing)
		return;

	if (theWords.size() >= MAX_WORDS)
	{
		theTruncated = true;
		return;
	}

	theWords.push_back(word);
	theCrcs.push_back(crcAfter);
}

void mark( const char *label )
{
	if (!theCapturing || label == nullptr)
		return;

	Mark m;
	m.pos = theWords.size();
	m.label = label;
	theMarks.push_back(m);
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

void report( UnsignedInt theirCRC, UnsignedInt ourCRC )
{
	if (!GXTrace::isNetEnabled())
		return;

	if (theWords.empty())
	{
		GX_NET_TRACE("crc locate frame %u: nothing captured -- the compared checksum"
			" was generated before capture was armed\n", (unsigned)theFrame);
		return;
	}

	if (theTruncated)
	{
		GX_NET_TRACE("crc locate frame %u: capture hit its %u-word cap, so this"
			" covers only the first part of the checksum\n",
			(unsigned)theFrame, (unsigned)MAX_WORDS);
	}

	// getCRC() byte-swaps on the way out; the walk works on the accumulator's own
	// value, so undo it.
	const UnsignedInt theirInternal = htobe(theirCRC);
	const UnsignedInt ourInternal = htobe(ourCRC);

	if (theCrcs.back() != ourInternal)
	{
		GX_NET_TRACE("crc locate frame %u: the captured stream ends at %08X but the"
			" checksum compared was %08X, so the capture belongs to a different"
			" computation. Not locating.\n",
			(unsigned)theFrame, (unsigned)htobe(theCrcs.back()), (unsigned)ourCRC);
		return;
	}

	const size_t n = theWords.size();

	// Inverse walk from their final value, using our words.
	std::vector<UnsignedInt> back(n + 1, 0);
	back[n] = theirInternal;
	for (size_t i = n; i > 0; --i)
		back[i - 1] = ror1(back[i] - theWords[i - 1]);

	std::vector<Candidate> cands;
	for (size_t i = 0; i < n; ++i)
	{
		const UnsignedInt ourPrev = (i >= 1) ? theCrcs[i - 1] : 0u;
		const UnsignedInt implied = back[i + 1] - rol1(ourPrev);

		Int delta = 0;
		if (looksLikeRounding(fieldBits(theWords[i]), fieldBits(implied), &delta))
		{
			Candidate c;
			c.index = i;
			c.impliedWord = implied;
			c.mantissaDelta = delta;
			cands.push_back(c);
		}
	}

	GX_NET_TRACE("crc locate frame %u: ours=%08X theirs=%08X over %u words;"
		" %u positions could be a rounding difference\n",
		(unsigned)theFrame, (unsigned)ourCRC, (unsigned)theirCRC,
		(unsigned)n, (unsigned)cands.size());

	if (cands.empty())
	{
		GX_NET_TRACE("crc locate frame %u: none. So the difference is not a rounded"
			" float -- an integer field, a status bit, a different object count, or"
			" more than one word apart. The stream length itself is the next thing"
			" to check against the other machine.\n", (unsigned)theFrame);
		return;
	}

	std::sort(cands.begin(), cands.end(), byMantissaDelta);

	size_t lo = cands.front().index;
	size_t hi = cands.front().index;
	for (size_t k = 0; k < cands.size(); ++k)
	{
		lo = std::min(lo, cands[k].index);
		hi = std::max(hi, cands[k].index);
	}
	size_t offLo = 0, offHi = 0;
	const char *labelLo = labelForPosition(lo, &offLo);
	const char *labelHi = labelForPosition(hi, &offHi);
	GX_NET_TRACE("crc locate frame %u: they all fall between word %u (%s +%u) and"
		" word %u (%s +%u)\n",
		(unsigned)theFrame, (unsigned)lo, labelLo, (unsigned)offLo,
		(unsigned)hi, labelHi, (unsigned)offHi);

	const size_t show = std::min(cands.size(), MAX_REPORTED);
	for (size_t k = 0; k < show; ++k)
	{
		const Candidate &c = cands[k];
		const UnsignedInt ourBits = fieldBits(theWords[c.index]);
		const UnsignedInt theirBits = fieldBits(c.impliedWord);
		size_t offsetWithin = 0;
		const char *label = labelForPosition(c.index, &offsetWithin);
		GX_NET_TRACE("crc locate frame %u:   #%u word %u  %s +%u  ours=%08X %.9g"
			"  theirs=%08X %.9g  mantissa %d apart\n",
			(unsigned)theFrame, (unsigned)(k + 1), (unsigned)c.index,
			label, (unsigned)offsetWithin,
			(unsigned)ourBits, asFloat(ourBits),
			(unsigned)theirBits, asFloat(theirBits),
			(int)c.mantissaDelta);
	}
}

} // namespace GXCrcStream
