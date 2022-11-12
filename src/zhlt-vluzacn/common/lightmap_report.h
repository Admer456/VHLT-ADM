
#pragma once

enum LightmapReportLevels
{
	LMREPORT_NONE = 0,
	LMREPORT_BASIC,
	LMREPORT_VERBOSE
};

// Handled in BSP and RAD
inline int g_lightmapReportLevel = LMREPORT_NONE;
