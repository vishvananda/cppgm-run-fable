#pragma once

extern int released;

bool use_after();

struct Lock
{
	int held;

	Lock() { held = 1; }
	~Lock() { released = 1; }

	bool ok() const { return held == 1; }
};
