/*
 * Copyright 2026, Kris Beazley
 * All rights reserved. Distributed under the terms of the MIT license.
 */
#ifndef HDESKBAR_CPU_H
#define HDESKBAR_CPU_H

#include <cstdint>
#include <utility>
#include <vector>

// The load of each processor, from /proc/stat: call Sample() now and then, and the loads are those since the last call.
class CpuSampler {
public:
	void Sample();
	float Average() const;

	std::vector<float> coreLoads;	// 0 to 1, one for each processor

private:
	std::vector<std::pair<uint64_t, uint64_t> > fPrevious;	// (busy, total) at the last sample
};

#endif
