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

////////////////////////////////////////////////////////////////////////////////
//																																						//
//  (c) 2001-2003 Electronic Arts Inc.																				//
//																																						//
////////////////////////////////////////////////////////////////////////////////

// FILE: PartitionSolver.cpp //////////////////////////////////////////////////////////////////////
/*---------------------------------------------------------------------------*/
/* EA Pacific                                                                */
/* Confidential Information	                                                 */
/* Copyright (C) 2001 - All Rights Reserved                                  */
/* DO NOT DISTRIBUTE                                                         */
/*---------------------------------------------------------------------------*/
/* Project:    RTS3                                                          */
/* File name:  PartitionSolver.cpp                                           */
/* Created:    John K. McDonald, Jr., 4/2/2002                               */
/* Desc:       This contains a general-purpose Partition solver							 */
/* Revision History:                                                         */
/*		4/12/2002 : Initial creation                                           */
/*---------------------------------------------------------------------------*/
/**************************************************************************************************
Some info about partitioning problems:

	This problem is contained in a very interesting class of problems known as NP complete. The
	basic problem is that there is no way to tell whether you have an optimal solution or not.
	Worst case, you try out every possible solution and still don't find the optimal solution:
	this takes 2^n time to find, where N is the number of elements you are attempting to place.
	For this reason, a value near PREFER_FAST_SOLUTION should almost always be chosen. We will use
	a flat multiply to determine how many solutions to attempt before giving up and returning our
	best attempt. If you want more info, this site contains info on the problem:
	http://odysseus.nat.uni-magdeburg.de/~mertens/npp/index.shtml
**************************************************************************************************/

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/PartitionSolver.h"
#include <algorithm>
#include "GameLogic/GameLogic.h"
#include "GXTrace.h"

static Bool greater_than(PairObjectIDAndUInt a, PairObjectIDAndUInt b)
{
	return a.second > b.second;
}

static void gxSortLikeMsvc(std::vector<PairObjectIDAndUInt> &v, const char *what)
{
	const size_t MSVC_INSERTION_SORT_MAX = 32;
	if (v.size() > MSVC_INSERTION_SORT_MAX)
		GX_NET_TRACE("partition solver frame %u: %u %s to sort, above MSVC's insertion-sort size; order of equal sizes may differ from the PC\n",
			(unsigned)TheGameLogic->getFrame(), (unsigned)v.size(), what);
	std::stable_sort(v.begin(), v.end(), greater_than);
}

PartitionSolver::PartitionSolver(const EntriesVec& elements, const SpacesVec& spaces, SolutionType solveHow)
{
	m_data = elements;
	m_spacesForData = spaces;
	m_howToSolve = solveHow;
	m_currentSolutionLeftovers = 0;
}

void PartitionSolver::solve()
{
	m_bestSolution.clear();
	m_currentSolution.clear();
	m_currentSolutionLeftovers = 0x7fffffff;

	Int minSizeForAllData = 0;
	Int slotsAllotted = 0;
	size_t i, j;

	// first, determine whether there is an actual solution, or we're going to have to fudge it.
	for (i = 0; i < m_data.size(); ++i) {
		minSizeForAllData += m_data[i].second;
	}

	for (i = 0; i < m_spacesForData.size(); ++i) {
		slotsAllotted += m_spacesForData[i].second;
	}

	// we want to attempt to place the largest things first. This allows us to throw
	// out whole classes of solutions

	// GeneralsX @bugfix Android port 24/09/2026 Sort like the PC client's STL.
	//
	// greater_than compares sizes only, so units (and transports) of equal size keep an
	// order that std::sort leaves unspecified, and that order decides which unit boards
	// which transport. The GeneralsOnline client is built with MSVC, whose std::sort is a
	// plain insertion sort for up to 32 elements: stable. libc++ sorts small ranges with
	// sorting networks that may reorder equal elements. std::stable_sort gives MSVC's order
	// exactly up to 32 elements; beyond that MSVC switches to an unstable quicksort, which
	// the trace flags because the phone cannot promise the PC's order there.
	gxSortLikeMsvc(m_data, "units");

	// Also make the largest partition first.
	gxSortLikeMsvc(m_spacesForData, "transports");

	// work in our temporary vector.
	SpacesVec spacesStillAvailable = m_spacesForData;

	if (m_howToSolve == PREFER_FAST_SOLUTION)
	{
		// we prefer the fast, but not necessarily correct solution
		// simply start placing the stuff. Skip things you can't place.
		for (i = 0; i < m_data.size(); ++i)
		{
			for (j = 0; j < spacesStillAvailable.size(); ++j)
			{
				if (m_data[i].second <= spacesStillAvailable[j].second)
				{
					spacesStillAvailable[j].second -= m_data[i].second;
					m_bestSolution.push_back(std::make_pair(m_data[i].first, spacesStillAvailable[j].first));
					break;
				}
			}
		}
	} else {
		DEBUG_CRASH(("PREFER_CORRECT_SOLUTION @todo impl"));
	}
}

const SolutionVec& PartitionSolver::getSolution() const
{
	return m_bestSolution;
}
