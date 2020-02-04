#include "gw2_table.hpp"
#include "filesystem.hpp"
#include "runtime.hpp"
#include "log.hpp"

gw2_table::gw2_table() {
	std::string path = reshade::runtime::s_gw2hook_wrkdir_path.string();
	fileBuffer = std::ifstream(path + "region.bin", std::ios::in | std::ios::binary);
	if (fileBuffer.is_open()) {
		fileBuffer.seekg(0, std::ios::end);
		region_file_lenght = (int)fileBuffer.tellg();
		LOG(INFO) << "Region index found. >" << path << ".";
	} else {
		region_file_lenght = 0;
		LOG(INFO) << "Region index not found! >" << path << ".";
	}
}

std::string gw2_table::get_region(int index) {
	if (region_file_lenght == 0 || index >= region_file_lenght || index <= 0) {
		return region[0];
	}
	char b;
	fileBuffer.seekg(index, std::ios::beg);
	fileBuffer.read(&b, 1);
	if (b >= region_lenght) {
		return region[0];
	}
	return region[b];
}

bool gw2_table::checkID(int n, std::string str){
	std::vector<int> vect;
	std::stringstream ss(str);
	int i;
	while (ss >> i) {
		vect.push_back(i);
		if (ss.peek() == '-')
			ss.ignore();
	}
	return (std::find(vect.begin(), vect.end(), n) != vect.end());
}

bool gw2_table::is_competitive(int n){
	return std::find(comp_map.begin(), comp_map.end(), n) != comp_map.end();
}
