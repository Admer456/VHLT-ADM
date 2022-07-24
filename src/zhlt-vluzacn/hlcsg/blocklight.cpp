// Blocklight texture related functions for CSG
// Admer456 (E-mail: admer456@gmail.com) (Discord: Admer456#2798)

#include "csg.h"
#include <vector>
#include <unordered_map>

#include "blocklight.hpp"

std::vector<brush_t> gBlockLightBrushes;
std::unordered_map<int, side_t> gBlockLightSides;

namespace BlockLight
{
	void MarkSide( int index, side_t* side )
	{
		//gBlockLightSides[index] = *side;
		//Log( "## Added side %i to blocklights\n", index );
	}

	void MarkBrush( brush_t* brush )
	{
		//gBlockLightBrushes.push_back( *brush );
		//Log( "## Added %ith brush to blocklights\n", gBlockLightBrushes.size() );
	}
}
