/*
 * Copyright 2026, Kris Beazley
 * All rights reserved. Distributed under the terms of the MIT license.
 */

#include "cpu.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <string>

void CpuSampler::Sample()
{
	std::ifstream stat("/proc/stat");
	std::vector<std::pair<uint64_t, uint64_t> > now;
	std::string line;
	while (std::getline(stat, line)) {
		if (line.compare(0, 3, "cpu") != 0 || line.size() < 4 || !isdigit(static_cast<unsigned char>(line[3])))
			continue;
		unsigned long long v[8] = { 0 };
		sscanf(line.c_str(), "%*s %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5],
			&v[6], &v[7]);
		const uint64_t idle = v[3] + v[4];
		uint64_t total = 0;
		for (int i = 0; i < 8; i++)
			total += v[i];
		now.push_back({ total - idle, total });
	}
	if (fPrevious.size() == now.size()) {
		coreLoads.resize(now.size());
		for (size_t i = 0; i < now.size(); i++) {
			const uint64_t total = now[i].second - fPrevious[i].second;
			const uint64_t busy = now[i].first - fPrevious[i].first;
			coreLoads[i] = total > 0 ? std::clamp(static_cast<float>(busy) / total, 0.0f, 1.0f) : 0.0f;
		}
	}
	fPrevious = now;
}

float CpuSampler::Average() const
{
	if (coreLoads.empty())
		return 0;
	float sum = 0;
	for (float load : coreLoads)
		sum += load;
	return sum / coreLoads.size();
}
