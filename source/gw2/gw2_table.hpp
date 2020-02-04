#pragma once

#include "log.hpp"
#include <fstream>
#include <sstream>
#include <vector>

class gw2_table {
public:
	gw2_table();
	std::string get_region(int index);
	static bool checkID(int n, std::string str);
	bool is_competitive(int n);
private:
	int region_file_lenght;
	std::ifstream fileBuffer;
	const int region_lenght = 13;
	//38, 95, 96, 899, 968, 1099 (McM)
	//350, 549, 554, 795, 875, 894, 900, 984, 1010, 1161, 1171, 1200
	const char* c_region[13] = {
		"global",
		"Shiverpeak Mountains",
		"Ascalon",
		"Ruins of Orr",
		"Kryta",
		"Tarnished Coast",
		"Player vs. Player",
		"World vs. World",
		"global",
		"Sea of Sorrows",
		"Heart of Maguuma",
		"Maguuma Wastes",
		"Crystal Desert",
	};
	const std::string region[13] = {
		c_region[0],
		c_region[1],
		c_region[2],
		c_region[3],
		c_region[4],
		c_region[5],
		c_region[6],
		c_region[7],
		c_region[8],
		c_region[9],
		c_region[10],
		c_region[11],
		c_region[12],
	};
	const std::vector<int> comp_map = {
		38, 95, 96, 350, 549, 554, 795, 875, 894, 899, 900, 968, 984, 1011, 1099, 1163, 1171, 1200
	};
};	

