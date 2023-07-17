#include "csg.h"

plane_t         g_mapplanes[MAX_INTERNAL_MAP_PLANES];
int             g_nummapplanes;
hullshape_t		g_defaulthulls[NUM_HULLS];
int				g_numhullshapes;
hullshape_t		g_hullshapes[MAX_HULLSHAPES];

#define DIST_EPSILON   0.04


// =====================================================================================
//  FindIntPlane, fast version (replacement by KGP)
//	This process could be optimized by placing the planes in a (non hash-) set and using
//	half of the inner loop check below as the comparator; I'd expect the speed gain to be
//	very large given the change from O(N^2) to O(NlogN) to build the set of planes.
// =====================================================================================

int FindIntPlane(const vec_t* const normal, const vec_t* const origin)
{
    int             returnval;
    plane_t*        p;
    plane_t         temp;
    vec_t           t;

	returnval = 0;

	find_plane:
	for( ; returnval < g_nummapplanes; returnval++)
	{
		// BUG: there might be some multithread issue --vluzacn
		if(	-DIR_EPSILON < (t = normal[0] - g_mapplanes[returnval].normal[0]) && t < DIR_EPSILON &&
			-DIR_EPSILON < (t = normal[1] - g_mapplanes[returnval].normal[1]) && t < DIR_EPSILON &&
			-DIR_EPSILON < (t = normal[2] - g_mapplanes[returnval].normal[2]) && t < DIR_EPSILON )
		{
			t = DotProduct (origin, g_mapplanes[returnval].normal) - g_mapplanes[returnval].dist;

			if (-DIST_EPSILON < t && t < DIST_EPSILON)
			{ return returnval; }
		}
	}

	ThreadLock();
	if(returnval != g_nummapplanes) // make sure we don't race
	{
		ThreadUnlock();
		goto find_plane; //check to see if other thread added plane we need
	}

    // create new planes - double check that we have room for 2 planes
    hlassume(g_nummapplanes+1 < MAX_INTERNAL_MAP_PLANES, assume_MAX_INTERNAL_MAP_PLANES);

	p = &g_mapplanes[g_nummapplanes];

	VectorCopy(origin,p->origin);
	VectorCopy(normal,p->normal);
    VectorNormalize(p->normal);
	p->type = PlaneTypeForNormal(p->normal);
	if (p->type <= last_axial)
	{
		for (int i = 0; i < 3; i++)
		{
			if (i == p->type)
				p->normal[i] = p->normal[i] > 0? 1: -1;
			else
				p->normal[i] = 0;
		}
	}
    p->dist = DotProduct(origin, p->normal);

	VectorCopy(origin,(p+1)->origin);
	VectorSubtract(vec3_origin,p->normal,(p+1)->normal);
	(p+1)->type = p->type;
	(p+1)->dist = -p->dist;

    // always put axial planes facing positive first
	if (normal[(p->type)%3] < 0)
	{
		temp = *p;
		*p = *(p+1);
		*(p+1) = temp;
        returnval = g_nummapplanes+1;
	}
	else
	{ returnval = g_nummapplanes; }

	g_nummapplanes += 2;
	ThreadUnlock();
	return returnval;
}



int PlaneFromPoints(const vec_t* const p0, const vec_t* const p1, const vec_t* const p2)
{
    vec3_t          v1, v2;
    vec3_t          normal;

    VectorSubtract(p0, p1, v1);
    VectorSubtract(p2, p1, v2);
    CrossProduct(v1, v2, normal);
    if (VectorNormalize(normal))
    {
        return FindIntPlane(normal, p0);
    }
    return -1;
}


const char ClipTypeStrings[5][11] = {{"smallest"},{"normalized"},{"simple"},{"precise"},{"legacy"}};

const char* GetClipTypeString(cliptype ct)
{
	return ClipTypeStrings[ct];
}


// =====================================================================================
//  AddHullPlane (subroutine for replacement of ExpandBrush, KGP)
//  Called to add any and all clip hull planes by the new ExpandBrush.
// =====================================================================================

void AddHullPlane(brushhull_t* hull, const vec_t* const normal, const vec_t* const origin, const bool check_planenum, int texInfo = -1)
{
	int planenum = FindIntPlane(normal,origin);
	//check to see if this plane is already in the brush (optional to speed
	//up cases where we know the plane hasn't been added yet, like axial case)
	if(check_planenum)
	{

		bface_t* current_face;
		for(current_face = hull->faces; current_face; current_face = current_face->next)
		{
			if(current_face->planenum == planenum)
			{ return; } //don't add a plane twice
		}
	}
	bface_t* new_face = (bface_t*)Alloc(sizeof(bface_t)); // TODO: This leaks
	new_face->planenum = planenum;
	new_face->plane = &g_mapplanes[new_face->planenum];
	new_face->next = hull->faces;
	new_face->contents = CONTENTS_EMPTY;
	new_face->texinfo = texInfo;
	hull->faces = new_face;
}

// =====================================================================================
//  ExpandBrush (replacement by KGP)
//  Since the six bounding box planes were always added anyway, they've been moved to
//  an explicit separate step eliminating the need to check for duplicate planes (which
//  should be using plane numbers instead of the full definition anyway).
//
//  The core of the new function adds additional bevels to brushes containing faces that
//  have 3 nonzero normal components -- this is necessary to finish the beveling process,
//  but is turned off by default for backward compatability and because the number of
//  clipnodes and faces will go up with the extra beveling.  The advantage of the extra
//  precision comes from the absense of "sticky" outside corners on ackward geometry.
//
//  Another source of "sticky" walls has been the inconsistant offset along each axis
//  (variant with plane normal in the old code).  The normal component of the offset has
//  been scrapped (it made a ~29% difference in the worst case of 45 degrees, or about 10
//  height units for a standard half-life player hull).  The new offsets generate fewer
//  clipping nodes and won't cause players to stick when moving across 2 brushes that flip
//  sign along an axis (this wasn't noticible on floors because the engine took care of the
//  invisible 0-3 unit steps it generated, but was noticible with walls).
//
//  To prevent players from floating 10 units above the floor, the "precise" hull generation
//  option still uses the plane normal when the Z component is high enough for the plane to
//  be considered a floor.  The "simple" hull generation option always uses the full hull
//  distance, resulting in lower clipnode counts.
//
//  Bevel planes might be added twice (once from each side of the edge), so a planenum
//  based check is used to see if each has been added before.
// =====================================================================================
// Correction: //--vluzacn
//   Clipnode size depends on complexity of the surface of expanded brushes as a whole, not number of brush sides.
//   Data from a sample map:
//     cliptype          simple    precise     legacy normalized   smallest
//     clipnodecount        971       1089       1202       1232       1000

void ExpandBrushWithHullBrush (const brush_t *brush, const brushhull_t *hull0, const hullbrush_t *hullBrush, brushhull_t *hull)
{
	const hullbrushface_t *hullBrushFace;
	const hullbrushedge_t *hullBrushEdge;
	const hullbrushvertex_t *hullBrushVertex;
	bface_t *f;
	vec3_t normal;
	vec3_t origin;
	bool *axialbevel;
	bool warned;

	axialbevel = (bool *)malloc (hullBrush->numfaces * sizeof (bool));
	memset (axialbevel, 0, hullBrush->numfaces * sizeof (bool));
	warned = false;
	
	// check for collisions of face-vertex type. face-edge type is also permitted. face-face type is excluded.
	for (f = hull0->faces; f; f = f->next)
	{
		hullbrushface_t brushface;
		VectorCopy (f->plane->normal, brushface.normal);
		VectorCopy (f->plane->origin, brushface.point);

		// check for coplanar hull brush face
		for (hullBrushFace = hullBrush->faces; hullBrushFace < hullBrush->faces + hullBrush->numfaces; hullBrushFace++)
		{
			if (-DotProduct (hullBrushFace->normal, brushface.normal) < 1 - ON_EPSILON)
			{
				continue;
			}
			// now test precisely
			vec_t dotmin;
			vec_t dotmax;
			dotmin = BOGUS_RANGE;
			dotmax = -BOGUS_RANGE;
			hlassume (hullBrushFace->numvertexes >= 1, assume_first);
			for (vec3_t *v = hullBrushFace->vertexes; v < hullBrushFace->vertexes + hullBrushFace->numvertexes; v++)
			{
				vec_t dot;
				dot = DotProduct (*v, brushface.normal);
				dotmin = qmin (dotmin, dot);
				dotmax = qmax (dotmax, dot);
			}
			if (dotmax - dotmin <= EQUAL_EPSILON)
			{
				break;
			}
		}
		if (hullBrushFace < hullBrush->faces + hullBrush->numfaces)
		{
			if (f->bevel)
			{
				axialbevel[hullBrushFace - hullBrush->faces] = true;
			}
			continue; // the same plane will be added in the last stage
		}
		
		// find the impact point
		vec3_t bestvertex;
		vec_t bestdist;
		bestdist = BOGUS_RANGE;
		hlassume (hullBrush->numvertexes >= 1, assume_first);
		for (hullBrushVertex = hullBrush->vertexes; hullBrushVertex < hullBrush->vertexes + hullBrush->numvertexes; hullBrushVertex++)
		{
			if (hullBrushVertex == hullBrush->vertexes || DotProduct (hullBrushVertex->point, brushface.normal) < bestdist - NORMAL_EPSILON)
			{
				bestdist = DotProduct (hullBrushVertex->point, brushface.normal);
				VectorCopy (hullBrushVertex->point, bestvertex);
			}
		}

		// add hull plane for this face
		VectorCopy (brushface.normal, normal);
		if (f->bevel)
		{
			VectorCopy (brushface.point, origin);
		}
		else
		{
			VectorSubtract (brushface.point, bestvertex, origin);
		}
		AddHullPlane (hull, normal, origin, true, f->texinfo);
	}

	// check for edge-edge type. edge-face type and face-edge type are excluded.
	for (f = hull0->faces; f; f = f->next)
	{
		for (int i = 0; i < f->w->m_NumPoints; i++) // for each edge in f
		{
			hullbrushedge_t brushedge;
			VectorCopy (f->plane->normal, brushedge.normals[0]);
			VectorCopy (f->w->m_Points[(i + 1) % f->w->m_NumPoints], brushedge.vertexes[0]);
			VectorCopy (f->w->m_Points[i], brushedge.vertexes[1]);
			VectorCopy (brushedge.vertexes[0], brushedge.point);
			VectorSubtract (brushedge.vertexes[1], brushedge.vertexes[0], brushedge.delta);

			// fill brushedge.normals[1]
			int found;
			found = 0;
			for (bface_t *f2 = hull0->faces; f2; f2 = f2->next)
			{
				for (int j = 0; j < f2->w->m_NumPoints; j++)
				{
					if (VectorCompare (f2->w->m_Points[(j + 1) % f2->w->m_NumPoints], brushedge.vertexes[1]) &&
						VectorCompare (f2->w->m_Points[j], brushedge.vertexes[0]))
					{
						VectorCopy (f2->plane->normal, brushedge.normals[1]);
						found++;
					}
				}
			}
			if (found != 1)
			{
				if (!warned)
				{
					Warning("Illegal Brush (edge without opposite face): Entity %i, Brush %i\n",
						brush->originalentitynum, brush->originalbrushnum
						);
					warned = true;
				}
				continue;
			}

			// make sure the math is accurate
			vec_t len;
			len = VectorLength (brushedge.delta);
			CrossProduct (brushedge.normals[0], brushedge.normals[1], brushedge.delta);
			if (!VectorNormalize (brushedge.delta))
			{
				continue;
			}
			VectorScale (brushedge.delta, len, brushedge.delta);

			// check for each edge in the hullbrush
			for (hullBrushEdge = hullBrush->edges; hullBrushEdge < hullBrush->edges + hullBrush->numedges; hullBrushEdge++)
			{
				vec_t dot[4];
				dot[0] = DotProduct (hullBrushEdge->delta, brushedge.normals[0]);
				dot[1] = DotProduct (hullBrushEdge->delta, brushedge.normals[1]);
				dot[2] = DotProduct (brushedge.delta, hullBrushEdge->normals[0]);
				dot[3] = DotProduct (brushedge.delta, hullBrushEdge->normals[1]);
				if (dot[0] <= ON_EPSILON || dot[1] >= -ON_EPSILON || dot[2] <= ON_EPSILON || dot[3] >= -ON_EPSILON)
				{
					continue;
				}

				// in the outer loop, each edge in the brush will be iterated twice (once from f and once from the corresponding f2)
				// but since brushedge.delta are exactly the opposite between the two iterations
				// only one of them can reach here
				vec3_t e1;
				vec3_t e2;
				VectorCopy (brushedge.delta, e1);
				VectorNormalize (e1);
				VectorCopy (hullBrushEdge->delta, e2);
				VectorNormalize (e2);
				CrossProduct (e1, e2, normal);
				if (!VectorNormalize (normal))
				{
					continue;
				}
				VectorSubtract (brushedge.point, hullBrushEdge->point, origin);
				AddHullPlane (hull, normal, origin, true, f->texinfo);
			}
		}
	}

	// check for vertex-face type. edge-face type and face-face type are permitted.
	for (hullBrushFace = hullBrush->faces; hullBrushFace < hullBrush->faces + hullBrush->numfaces; hullBrushFace++)
	{
		// find the impact point
		vec3_t bestvertex;
		vec_t bestdist;
		bestdist = BOGUS_RANGE;
		int texInfo = -1;
		if (!hull0->faces)
		{
			continue;
		}
		for (f = hull0->faces; f; f = f->next)
		{
			for (vec3_t *v = f->w->m_Points; v < f->w->m_Points + f->w->m_NumPoints; v++)
			{
				if (DotProduct (*v, hullBrushFace->normal) < bestdist - NORMAL_EPSILON)
				{
					texInfo = f->texinfo;
					bestdist = DotProduct (*v, hullBrushFace->normal);
					VectorCopy (*v, bestvertex);
				}
			}
		}

		// add hull plane for this face
		VectorSubtract (vec3_origin, hullBrushFace->normal, normal);
		if (axialbevel[hullBrushFace - hullBrush->faces])
		{
			VectorCopy (bestvertex, origin);
		}
		else
		{
			VectorSubtract (bestvertex, hullBrushFace->point, origin);
		}
		AddHullPlane (hull, normal, origin, true, texInfo);
	}

	free (axialbevel);
}

void ExpandBrush(brush_t* brush, const int hullnum)
{
	const hullshape_t *hs = &g_defaulthulls[hullnum];
	{ // look up the name of its hull shape in g_hullshapes[]
		const char *name = brush->hullshapes[hullnum];
		if (name && *name)
		{
			bool found = false;
			for (int i = 0; i < g_numhullshapes; i++)
			{
				const hullshape_t *s = &g_hullshapes[i];
				if (!strcmp (name, s->id))
				{
					if (found)
					{
						Warning ("Entity %i, Brush %i: Found several info_hullshape entities with the same name '%s'.",
							brush->originalentitynum, brush->originalbrushnum,
							name);
					}
					hs = s;
					found = true;
				}
			}
			if (!found)
			{
				Error ("Entity %i, Brush %i: Couldn't find info_hullshape entity '%s'.",
					brush->originalentitynum, brush->originalbrushnum,
					name);
			}
		}
	}

	if (!hs->disabled)
	{
		if (hs->numbrushes == 0)
		{
			return; // leave this hull of this brush empty (noclip)
		}
		ExpandBrushWithHullBrush (brush, &brush->hulls[0], hs->brushes[0], &brush->hulls[hullnum]);

		return;
	}
	//for looping through the faces and constructing the hull
	bface_t* current_face;
	plane_t* current_plane;
	brushhull_t* hull;
	vec3_t	origin, normal;

	//for non-axial bevel testing
	Winding* winding;
	bface_t* other_face;
	plane_t* other_plane;
	Winding* other_winding;
	vec3_t  edge_start, edge_end, edge, bevel_edge;
	unsigned int counter, counter2, dir;
	bool start_found,end_found;
	bool axialbevel[last_axial+1][2] = { {false,false}, {false,false}, {false,false} };

	bool warned = false;

	hull = &brush->hulls[hullnum];

	// step 1: for collision between player vertex and brush face. --vluzacn
	for(current_face = brush->hulls[0].faces; current_face; current_face = current_face->next)
	{
		current_plane = current_face->plane;

		//don't bother adding axial planes,
		//they're defined by adding the bounding box anyway
		if(current_plane->type <= last_axial)
		{
			//flag case where bounding box shouldn't expand
			if (current_face->bevel)
			{
				switch(current_plane->type)
				{
				case plane_x:
					axialbevel[plane_x][(current_plane->normal[0] > 0 ? 1 : 0)] = true;
					break;
				case plane_y:
					axialbevel[plane_y][(current_plane->normal[1] > 0 ? 1 : 0)] = true;
					break;
				case plane_z:
					axialbevel[plane_z][(current_plane->normal[2] > 0 ? 1 : 0)] = true;
					break;
				}
			}
			continue;
		}

		//add the offset non-axial plane to the expanded hull
        VectorCopy(current_plane->origin, origin);
        VectorCopy(current_plane->normal, normal);

		//old code multiplied offset by normal -- this led to post-csg "sticky" walls where a
		//slope met an axial plane from the next brush since the offset from the slope would be less
		//than the full offset for the axial plane -- the discontinuity also contributes to increased
		//clipnodes.  If the normal is zero along an axis, shifting the origin in that direction won't
		//change the plane number, so I don't explicitly test that case.  The old method is still used if
		//preciseclip is turned off to allow backward compatability -- some of the improperly beveled edges
		//grow using the new origins, and might cause additional problems.

		if (current_face->bevel)
		{
			//don't adjust origin - we'll correct g_texinfo's flags in a later step
		}
		// The old offset will generate an extremely small gap when the normal is close to axis, causing epsilon errors (ambiguous leafnode content, player falling into ground, etc.).
		// For example: with the old shifting method, slopes with angle arctan(1/8) and arctan(1/64) will result in gaps of 0.0299 unit and 0.000488 unit respectively, which are smaller than ON_EPSILON, while in both 'simple' cliptype and the new method, the gaps are 2.0 units and 0.25 unit, which are good.
		// This also reduce the number of clipnodes used for cliptype precise.
		// The maximum difference in height between the old offset and the new offset is 0.86 unit for standing player and 6.9 units for ducking player. (when FLOOR_Z = 0.7)
		// And another reason not to use the old offset is that the old offset is quite wierd. It might appears at first glance that it regards the player as an ellipse, but in fact it isn't, and the player's feet may even sink slightly into the ground theoretically for slopes of certain angles.
		else if (g_cliptype == clip_precise && normal[2] > FLOOR_Z)
		{
			origin[0] += 0;
			origin[1] += 0;
			origin[2] += g_hull_size[hullnum][1][2];
		}
		else if (g_cliptype == clip_legacy || g_cliptype == clip_normalized)
		{
			if(normal[0])
			{ origin[0] += normal[0] * (normal[0] > 0 ? g_hull_size[hullnum][1][0] : -g_hull_size[hullnum][0][0]); }
			if(normal[1])
			{ origin[1] += normal[1] * (normal[1] > 0 ? g_hull_size[hullnum][1][1] : -g_hull_size[hullnum][0][1]); }
			if(normal[2])
			{ origin[2] += normal[2] * (normal[2] > 0 ? g_hull_size[hullnum][1][2] : -g_hull_size[hullnum][0][2]); }
		}
		else
		{
			origin[0] += g_hull_size[hullnum][(normal[0] > 0 ? 1 : 0)][0];
			origin[1] += g_hull_size[hullnum][(normal[1] > 0 ? 1 : 0)][1];
			origin[2] += g_hull_size[hullnum][(normal[2] > 0 ? 1 : 0)][2];
		}

		AddHullPlane(hull,normal,origin,false, current_face->texinfo);
	} //end for loop over all faces

	// step 2: for collision between player edge and brush edge. --vluzacn

	//split bevel check into a second pass so we don't have to check for duplicate planes when adding offset planes
	//in step above -- otherwise a bevel plane might duplicate an offset plane, causing problems later on.

	//only executes if cliptype is simple, normalized or precise
	if(g_cliptype == clip_simple || g_cliptype == clip_precise || g_cliptype == clip_normalized)
	{
		for(current_face = brush->hulls[0].faces; current_face; current_face = current_face->next)
		{
			current_plane = current_face->plane;

			//test to see if the plane is completely non-axial (if it is, need to add bevels to any
			//existing "inflection edges" where there's a sign change with a neighboring plane's normal for
			//a given axis)

			//move along winding and find plane on other side of each edge.  If normals change sign,
			//add a new plane by offsetting the points of the winding to bevel the edge in that direction.
			//It's possible to have inflection in multiple directions -- in this case, a new plane
			//must be added for each sign change in the edge.

			winding = current_face->w;

			for(counter = 0; counter < (winding->m_NumPoints); counter++) //for each edge
			{
				VectorCopy(winding->m_Points[counter],edge_start);
				VectorCopy(winding->m_Points[(counter+1)%winding->m_NumPoints],edge_end);

				//grab the edge (find relative length)
				VectorSubtract(edge_end,edge_start,edge);

				//brute force - need to check every other winding for common points -- if the points match, the
				//other face is the one we need to look at.
				for(other_face = brush->hulls[0].faces; other_face; other_face = other_face->next)
				{
					if(other_face == current_face)
					{ continue; }
					start_found = false;
					end_found = false;
					other_winding = other_face->w;
					for(counter2 = 0; counter2 < other_winding->m_NumPoints; counter2++)
					{
						if(!start_found && VectorCompare(other_winding->m_Points[counter2],edge_start))
						{ start_found = true; }
						if(!end_found && VectorCompare(other_winding->m_Points[counter2],edge_end))
						{ end_found = true; }
						if(start_found && end_found)
						{ break; } //we've found the face we want, move on to planar comparison
					} // for each point in other winding
					if(start_found && end_found)
					{ break; } //we've found the face we want, move on to planar comparison
				} // for each face

				if(!other_face)
				{
					if(hullnum == 1 && !warned)
					{
						Warning("Illegal Brush (edge without opposite face): Entity %i, Brush %i\n",
							brush->originalentitynum, brush->originalbrushnum
							);
						warned = true;
					}
					continue;
				}

				other_plane = other_face->plane;


				//check each direction for sign change in normal -- zero can be safely ignored
				for(dir = 0; dir < 3; dir++)
				{
					if(current_plane->normal[dir]*other_plane->normal[dir] < -NORMAL_EPSILON) //sign changed, add bevel
					{
						//pick direction of bevel edge by looking at normal of existing planes
						VectorClear(bevel_edge);
						bevel_edge[dir] = (current_plane->normal[dir] > 0) ? -1 : 1;

						//find normal by taking normalized cross of the edge vector and the bevel edge
						CrossProduct(edge,bevel_edge,normal);

						//normalize to length 1
						VectorNormalize(normal);
						if (fabs (normal[(dir+1)%3]) <= NORMAL_EPSILON || fabs (normal[(dir+2)%3]) <= NORMAL_EPSILON)
						{ // coincide with axial plane
							continue;
						}

						//get the origin
						VectorCopy(edge_start,origin);

						//unrolled loop - legacy never hits this point, so don't test for it
						if (g_cliptype == clip_precise && normal[2] > FLOOR_Z)
						{
							origin[0] += 0;
							origin[1] += 0;
							origin[2] += g_hull_size[hullnum][1][2];
						}
						else if (g_cliptype == clip_normalized)
						{
							if(normal[0])
							{ origin[0] += normal[0] * (normal[0] > 0 ? g_hull_size[hullnum][1][0] : -g_hull_size[hullnum][0][0]); }
							if(normal[1])
							{ origin[1] += normal[1] * (normal[1] > 0 ? g_hull_size[hullnum][1][1] : -g_hull_size[hullnum][0][1]); }
							if(normal[2])
							{ origin[2] += normal[2] * (normal[2] > 0 ? g_hull_size[hullnum][1][2] : -g_hull_size[hullnum][0][2]); }
						}
						else //simple or precise for non-floors
						{
							//note: if normal == 0 in direction indicated, shifting origin doesn't change plane #
							origin[0] += g_hull_size[hullnum][(normal[0] > 0 ? 1 : 0)][0];
							origin[1] += g_hull_size[hullnum][(normal[1] > 0 ? 1 : 0)][1];
							origin[2] += g_hull_size[hullnum][(normal[2] > 0 ? 1 : 0)][2];
						}

						//add the bevel plane to the expanded hull
						AddHullPlane(hull,normal,origin,true, current_face->texinfo); //double check that this edge hasn't been added yet
					}
				} //end for loop (check for each direction)
			} //end for loop (over all edges in face)
		} //end for loop (over all faces in hull 0)
	} //end if completely non-axial

	// step 3: for collision between player face and brush vertex. --vluzacn

	//add the bounding box to the expanded hull -- for a
	//completely axial brush, this is the only necessary step

	//add mins
	VectorAdd(brush->hulls[0].bounds.m_Mins, g_hull_size[hullnum][0], origin);
	normal[0] = -1;
	normal[1] = 0;
	normal[2] = 0;
	AddHullPlane(hull,normal,(axialbevel[plane_x][0] ? brush->hulls[0].bounds.m_Mins : origin),false);
	normal[0] = 0;
	normal[1] = -1;
	AddHullPlane(hull,normal,(axialbevel[plane_y][0] ? brush->hulls[0].bounds.m_Mins : origin),false);
	normal[1] = 0;
	normal[2] = -1;
	AddHullPlane(hull,normal,(axialbevel[plane_z][0] ? brush->hulls[0].bounds.m_Mins : origin),false);

	normal[2] = 0;

	//add maxes
	VectorAdd(brush->hulls[0].bounds.m_Maxs, g_hull_size[hullnum][1], origin);
	normal[0] = 1;
	AddHullPlane(hull,normal,(axialbevel[plane_x][1] ? brush->hulls[0].bounds.m_Maxs : origin),false);
	normal[0] = 0;
	normal[1] = 1;
	AddHullPlane(hull,normal,(axialbevel[plane_y][1] ? brush->hulls[0].bounds.m_Maxs : origin),false);
	normal[1] = 0;
	normal[2] = 1;
	AddHullPlane(hull,normal,(axialbevel[plane_z][1] ? brush->hulls[0].bounds.m_Maxs : origin),false);
/*
	bface_t* hull_face; //sanity check

	for(hull_face = hull->faces; hull_face; hull_face = hull_face->next)
	{
		for(current_face = brush->hulls[0].faces; current_face; current_face = current_face->next)
		{
			if(current_face->w->m_NumPoints < 3)
			{ continue; }
			for(counter = 0; counter < current_face->w->m_NumPoints; counter++)
			{
				if(DotProduct(hull_face->plane->normal,hull_face->plane->origin) < DotProduct(hull_face->plane->normal,current_face->w->m_Points[counter]))
				{
					Warning("Illegal Brush (clip hull [%i] has backward face): Entity %i, Brush %i\n",hullnum,
#ifdef HLCSG_COUNT_NEW
						brush->originalentitynum, brush->originalbrushnum
#else
						brush->entitynum, brush->brushnum
#endif
						);
					break;
				}
			}
		}
	}
*/
}

// =====================================================================================
//  MakeHullFaces
// =====================================================================================
void SortSides (brushhull_t *h)
{
	int numsides;
	bface_t **sides;
	vec3_t *normals;
	bool *isused;
	int i, j;
	int *sorted;
	bface_t *f;
	for (numsides = 0, f = h->faces; f; f = f->next)
	{
		numsides++;
	}
	sides = (bface_t **)malloc (numsides * sizeof (bface_t *));
	hlassume (sides != NULL, assume_NoMemory);
	normals = (vec3_t *)malloc (numsides * sizeof (vec3_t));
	hlassume (normals != NULL, assume_NoMemory);
	isused = (bool *)malloc (numsides * sizeof (bool));
	hlassume (isused != NULL, assume_NoMemory);
	sorted = (int *)malloc (numsides * sizeof (int));
	hlassume (sorted != NULL, assume_NoMemory);
	for (i = 0, f = h->faces; f; i++, f = f->next)
	{
		sides[i] = f;
		isused[i] = false;
		const plane_t *p = &g_mapplanes[f->planenum];
		VectorCopy (p->normal, normals[i]);
	}
	for (i = 0; i < numsides; i++)
	{
		int bestside;
		int bestaxial = -1;
		for (j = 0; j < numsides; j++)
		{
			if (isused[j])
			{
				continue;
			}
			int axial = (fabs (normals[j][0]) < NORMAL_EPSILON) + (fabs (normals[j][1]) < NORMAL_EPSILON) + (fabs (normals[j][2]) < NORMAL_EPSILON);
			if (axial > bestaxial)
			{
				bestside = j;
				bestaxial = axial;
			}
		}
		sorted[i] = bestside;
		isused[bestside] = true;
	}
	for (i = -1; i < numsides; i++)
	{
		*(i >= 0? &sides[sorted[i]]->next: &h->faces) = (i + 1 < numsides? sides[sorted[i + 1]]: NULL);
	}
	free (sides);
	free (normals);
	free (isused);
	free (sorted);
}
void MakeHullFaces(const brush_t* const b, brushhull_t *h)
{
    bface_t*        f;
    bface_t*        f2;
	// this will decrease AllocBlock amount
	SortSides (h);

restart:
    h->bounds.reset();

    // for each face in this brushes hull
    for (f = h->faces; f; f = f->next)
    {
        Winding* w = new Winding(f->plane->normal, f->plane->dist);
        for (f2 = h->faces; f2; f2 = f2->next)
        {
            if (f == f2)
            {
                continue;
            }
            const plane_t* p = &g_mapplanes[f2->planenum ^ 1];
            if (!w->Chop(p->normal, p->dist
				, NORMAL_EPSILON  // fix "invalid brush" in ExpandBrush
				))   // Nothing left to chop (getArea will return 0 for us in this case for below)
            {
                break;
            }
        }
		w->RemoveColinearPoints (ON_EPSILON);
        if (w->getArea() < 0.1)
        {
            delete w;
            if (h->faces == f)
            {
                h->faces = f->next;
            }
            else
            {
                for (f2 = h->faces; f2->next != f; f2 = f2->next)
                {
                    ;
                }
                f2->next = f->next;
            }
            goto restart;
        }
        else
        {
            f->w = w;
            f->contents = CONTENTS_EMPTY;
            unsigned int    i;
            for (i = 0; i < w->m_NumPoints; i++)
            {
                h->bounds.add(w->m_Points[i]);
            }
        }
    }

    unsigned int    i;
    for (i = 0; i < 3; i++)
    {
        if (h->bounds.m_Mins[i] < -g_iMaxEntityRange || h->bounds.m_Maxs[i] > g_iMaxEntityRange)
        {
            Fatal(assume_BRUSH_OUTSIDE_WORLD, "Entity %i, Brush %i: outside world(+/-%d): (%.0f,%.0f,%.0f)-(%.0f,%.0f,%.0f)",
				b->originalentitynum, b->originalbrushnum,
				   g_iMaxEntityRange,
                  h->bounds.m_Mins[0], h->bounds.m_Mins[1], h->bounds.m_Mins[2],
                  h->bounds.m_Maxs[0], h->bounds.m_Maxs[1], h->bounds.m_Maxs[2]);
        }
    }
}

// =====================================================================================
//  MakeBrushPlanes
// =====================================================================================
bool            MakeBrushPlanes(brush_t* b)
{
    int             i;
    int             j;
    int             planenum;
    side_t*         s;
    bface_t*        f;
    vec3_t          origin;

    //
    // if the origin key is set (by an origin brush), offset all of the values
    //
    GetVectorForKey(&g_entities[b->entitynum], "origin", origin);

    //
    // convert to mapplanes
    //
    // for each side in this brush
    for (i = 0; i < b->numsides; i++)
    {
        s = &g_brushsides[b->firstside + i];
        for (j = 0; j < 3; j++)
        {
            VectorSubtract(s->planepts[j], origin, s->planepts[j]);
        }
        planenum = PlaneFromPoints(s->planepts[0], s->planepts[1], s->planepts[2]);
        if (planenum == -1)
        {
            Fatal(assume_PLANE_WITH_NO_NORMAL, "Entity %i, Brush %i, Side %i: plane with no normal", 
				b->originalentitynum, b->originalbrushnum
				, i);
        }

        //
        // see if the plane has been used already
        //
        for (f = b->hulls[0].faces; f; f = f->next)
        {
            if (f->planenum == planenum || f->planenum == (planenum ^ 1))
            {
                Fatal(assume_BRUSH_WITH_COPLANAR_FACES, "Entity %i, Brush %i, Side %i: has a coplanar plane at (%.0f, %.0f, %.0f), texture %s",
					b->originalentitynum, b->originalbrushnum
					  , i, s->planepts[0][0] + origin[0], s->planepts[0][1] + origin[1],
                      s->planepts[0][2] + origin[2], s->td.name);
            }
        }

        f = (bface_t*)Alloc(sizeof(*f)); // TODO: This leaks
		
        f->planenum = planenum;
        f->plane = &g_mapplanes[planenum];
        f->next = b->hulls[0].faces;
        b->hulls[0].faces = f;
        f->texinfo = g_onlyents ? 0 : TexinfoForBrushTexture(f->plane, &s->td, origin );
		f->bevel = b->bevel || s->bevel;
    }

    return true;
}


// =====================================================================================
//  TextureContents
// =====================================================================================
static contents_t TextureContents(const char* const name)
{
	if ( !strncasecmp( name, "contentsolid", 12 ) )
		return CONTENTS_SOLID;
	if ( !strncasecmp( name, "contentwater", 12 ) )
		return CONTENTS_WATER;
	if ( !strncasecmp( name, "contentempty", 12 ) )
		return CONTENTS_TOEMPTY;
	if ( !strncasecmp( name, "contentsky", 10 ) )
		return CONTENTS_SKY;
	if ( !strncasecmp( name, "sky", 3 ) )
		return CONTENTS_SKY;

// =====================================================================================
//Cpt_Andrew - Env_Sky Check
// =====================================================================================
	if ( !strncasecmp( name, "env_sky", 7 ) )
		return CONTENTS_SKY;
// =====================================================================================

    if (name[0] == '!') //optimized -- don't check for current unless it's liquid (KGP)
	{
		if ( !strncasecmp( name, "!cur_90", 7 ) )
			return CONTENTS_CURRENT_90;
		if ( !strncasecmp( name, "!cur_0", 6 ) )
			return CONTENTS_CURRENT_0;
		if ( !strncasecmp( name, "!cur_270", 8 ) )
			return CONTENTS_CURRENT_270;
		if ( !strncasecmp( name, "!cur_180", 8 ) )
			return CONTENTS_CURRENT_180;
		if ( !strncasecmp( name, "!cur_up", 7 ) )
			return CONTENTS_CURRENT_UP;
		if ( !strncasecmp( name, "!cur_dwn", 8 ) )
			return CONTENTS_CURRENT_DOWN;

		if ( !strncasecmp( name + 1, "!lava", 5 ) )
			return CONTENTS_LAVA;
		if ( !strncasecmp( name + 1, "!slime", 6 ) )
			return CONTENTS_SLIME;

		if ( !strncasecmp( name, "!lava", 5 ) )
			return CONTENTS_LAVA;
		if ( !strncasecmp( name, "!slime", 6 ) )
			return CONTENTS_SLIME;

        return CONTENTS_WATER; //default for liquids
	}

	if ( !strncasecmp( name, "origin", 6 ) )
		return CONTENTS_ORIGIN;
	if ( !strncasecmp( name, "boundingbox", 11 ) )
		return CONTENTS_BOUNDINGBOX;

	if (!strncasecmp(name, "solidhint", 9))
		return CONTENTS_NULL;
	if ( !strncasecmp( name, "bolidhint", 9 ) )
		return CONTENTS_NULL;
	if ( !strcasecmp( name, "blocklight" ) )
		return CONTENTS_BLOCKLIGHT;
	if ( !strncasecmp( name, "splitface", 9 ) )
		return CONTENTS_HINT;
	if ( !strncasecmp( name, "hint", 4 ) )
		return CONTENTS_TOEMPTY;
	if ( !strncasecmp( name, "skip", 4 ) )
		return CONTENTS_TOEMPTY;

	if ( !strncasecmp( name, "translucent", 11 ) )
		return CONTENTS_TRANSLUCENT;

	if ( name[ 0 ] == '@' )
		return CONTENTS_TRANSLUCENT;

	if ( !strncasecmp( name, "null", 4 ) )
		return CONTENTS_NULL;
	if ( !strncasecmp( name, "bevel", 5 ) )
		return CONTENTS_NULL;

    return CONTENTS_SOLID;
}

// =====================================================================================
//  ContentsToString
// =====================================================================================
const char*     ContentsToString(const contents_t type)
{
    switch (type)
    {
    case CONTENTS_EMPTY:
        return "EMPTY";
    case CONTENTS_SOLID:
        return "SOLID";
    case CONTENTS_WATER:
        return "WATER";
    case CONTENTS_SLIME:
        return "SLIME";
    case CONTENTS_LAVA:
        return "LAVA";
    case CONTENTS_SKY:
        return "SKY";
    case CONTENTS_ORIGIN:
        return "ORIGIN";
	case CONTENTS_BOUNDINGBOX:
		return "BOUNDINGBOX";
    case CONTENTS_CURRENT_0:
        return "CURRENT_0";
    case CONTENTS_CURRENT_90:
        return "CURRENT_90";
    case CONTENTS_CURRENT_180:
        return "CURRENT_180";
    case CONTENTS_CURRENT_270:
        return "CURRENT_270";
    case CONTENTS_CURRENT_UP:
        return "CURRENT_UP";
    case CONTENTS_CURRENT_DOWN:
        return "CURRENT_DOWN";
    case CONTENTS_TRANSLUCENT:
        return "TRANSLUCENT";
    case CONTENTS_HINT:
        return "HINT";
	case CONTENTS_BLOCKLIGHT:
		return "BLOCKLIGHT";
    case CONTENTS_NULL:
        return "NULL";


	case CONTENTS_TOEMPTY:
		return "EMPTY";

    default:
        return "UNKNOWN";
    }
}

// =====================================================================================
//  CheckBrushContents
//      Perfoms abitrary checking on brush surfaces and states to try and catch errors
// =====================================================================================
contents_t      CheckBrushContents(const brush_t* const b)
{
    contents_t      best_contents;
    contents_t      contents;
    side_t*         s;
    int             i;
	int				best_i;
	bool			assigned = false;

    s = &g_brushsides[b->firstside];

    // cycle though the sides of the brush and attempt to get our best side contents for
    //  determining overall brush contents
	if (b->numsides == 0)
	{
		Error ("Entity %i, Brush %i: Brush with no sides.\n",
			b->originalentitynum, b->originalbrushnum
			);
	}
	best_i = 0;
    best_contents = TextureContents(s->td.name);

	// Difference between SKIP, ContentEmpty:
	// SKIP doesn't split space in bsp process, ContentEmpty splits space normally.
	if (!(strncasecmp (s->td.name, "content", 7) && strncasecmp (s->td.name, "skip", 4)))
		assigned = true;
    
	s++;
	
	for (i = 1; i < b->numsides; i++, s++)
    {
        contents_t contents_consider = TextureContents(s->td.name);
		if (assigned)
			continue;

		if (!(strncasecmp (s->td.name, "content", 7) && strncasecmp (s->td.name, "skip", 4)))
		{
			best_i = i;
			best_contents = contents_consider;
			assigned = true;
		}
        
		if (contents_consider > best_contents)
        {
			best_i = i;
            // if our current surface contents is better (larger) than our best, make it our best.
            best_contents = contents_consider;
        }
    }
    contents = best_contents;

    // attempt to pick up on mixed_face_contents errors
    s = &g_brushsides[b->firstside];
	for (i = 0; i < b->numsides; i++, s++)
    {
        contents_t contents2 = TextureContents(s->td.name);
		if (assigned
			&& strncasecmp (s->td.name, "content", 7)
			&& strncasecmp (s->td.name, "skip", 4)
			&& contents2 != CONTENTS_ORIGIN
			&& contents2 != CONTENTS_HINT
			&& contents2 != CONTENTS_BOUNDINGBOX
			)
		{
			continue; // overwrite content for this texture
		}

        // AJM: sky and null types are not to cause mixed face contents
        if (contents2 == CONTENTS_SKY)
            continue;

        if (contents2 == CONTENTS_NULL)
            continue;

        if (contents2 != best_contents)
        {
            Fatal(assume_MIXED_FACE_CONTENTS, "Entity %i, Brush %i: mixed face contents\n    Texture %s and %s",
				b->originalentitynum, b->originalbrushnum, 
                g_brushsides[b->firstside + best_i].td.name,
				s->td.name);
        }
    }
	if (contents == CONTENTS_NULL)
		contents = CONTENTS_SOLID;

    // check to make sure we dont have an origin brush as part of worldspawn
    if ((b->entitynum == 0) || (strcmp("func_group", ValueForKey(&g_entities[b->entitynum], "classname"))==0))
    {
        if (contents == CONTENTS_ORIGIN
				&& b->entitynum == 0
			|| contents == CONTENTS_BOUNDINGBOX
			)
        {
            Fatal(assume_BRUSH_NOT_ALLOWED_IN_WORLD, "Entity %i, Brush %i: %s brushes not allowed in world\n(did you forget to tie this origin brush to a rotating entity?)", 
				b->originalentitynum, b->originalbrushnum, 
				ContentsToString(contents));
        }
    }
    else
    {
        // otherwise its not worldspawn, therefore its an entity. check to make sure this brush is allowed
        //  to be an entity.
        switch (contents)
        {
        case CONTENTS_SOLID:
        case CONTENTS_WATER:
        case CONTENTS_SLIME:
        case CONTENTS_LAVA:
        case CONTENTS_ORIGIN:
		case CONTENTS_BOUNDINGBOX:
		case CONTENTS_HINT:
		case CONTENTS_BLOCKLIGHT:
		case CONTENTS_TOEMPTY:
            break;
        default:
            Fatal(assume_BRUSH_NOT_ALLOWED_IN_ENTITY, "Entity %i, Brush %i: %s brushes not allowed in entity", 
				b->originalentitynum, b->originalbrushnum, 
				ContentsToString(contents));
            break;
        }
    }

    return contents;
}


// =====================================================================================
//  CreateBrush
//      makes a brush!
// =====================================================================================
void CreateBrush(const int brushnum) //--vluzacn
{
	brush_t*        b;
	int             contents;
	int             h;

	b = &g_mapbrushes[brushnum];

	contents = b->contents;

	if (contents == CONTENTS_ORIGIN)
		return;
	if (contents == CONTENTS_BOUNDINGBOX)
		return;

	//  HULL 0
	MakeBrushPlanes(b);
	MakeHullFaces(b, &b->hulls[0]);

	if ( contents == CONTENTS_HINT || contents == CONTENTS_TOEMPTY || contents == CONTENTS_BLOCKLIGHT )
		return;

	if (g_noclip)
	{
		if (b->cliphull)
		{
			b->hulls[0].faces = NULL;
		}
		return;
	}

	if (b->cliphull)
	{
		for (h = 1; h < NUM_HULLS; h++)
		{
			if (b->cliphull & (1 << h))
			{
				ExpandBrush(b, h);
				MakeHullFaces(b, &b->hulls[h]);
			}
		}
        b->contents = CONTENTS_SOLID;
		b->hulls[0].faces = NULL;
	}
	else
	{
		if (b->noclip)
			return;
		for (h = 1; h < NUM_HULLS; h++)
		{
			ExpandBrush(b, h);
			MakeHullFaces(b, &b->hulls[h]);
		}
	}
}
hullbrush_t *CreateHullBrush (const brush_t *b)
{
	const int MAXSIZE = 256;

	hullbrush_t *hb;
	int numplanes;
	plane_t planes[MAXSIZE];
	Winding *w[MAXSIZE];
	int numedges;
	hullbrushedge_t edges[MAXSIZE];
	int numvertexes;
	hullbrushvertex_t vertexes[MAXSIZE];
	int i;
	int j;
	int k;
	int e;
	int e2;
	vec3_t origin;
	bool failed = false;

	// planes

	numplanes = 0;
	GetVectorForKey (&g_entities[b->entitynum], "origin", origin);

	for (i = 0; i < b->numsides; i++)
	{
		side_t *s;
		vec3_t p[3];
		vec3_t v1;
		vec3_t v2;
		vec3_t normal;
		planetypes axial;

		s = &g_brushsides[b->firstside + i];
		for (j = 0; j < 3; j++)
		{
			VectorSubtract (s->planepts[j], origin, p[j]);
			for (k = 0; k < 3; k++)
			{
				if (fabs (p[j][k] - floor (p[j][k] + 0.5)) <= ON_EPSILON && p[j][k] != floor (p[j][k] + 0.5))
				{
					Warning ("Entity %i, Brush %i: vertex (%4.8f %4.8f %4.8f) of an info_hullshape entity is slightly off-grid.",
							b->originalentitynum, b->originalbrushnum,
							p[j][0], p[j][1], p[j][2]);
				}
			}
		}
		VectorSubtract (p[0], p[1], v1);
		VectorSubtract (p[2], p[1], v2);
		CrossProduct (v1, v2, normal);
		if (!VectorNormalize (normal))
		{
			failed = true;
			continue;
		}
		for (k = 0; k < 3; k++)
		{
			if (fabs (normal[k]) < NORMAL_EPSILON)
			{
				normal[k] = 0.0;
				VectorNormalize (normal);
			}
		}
		axial = PlaneTypeForNormal (normal);
		if (axial <= last_axial)
		{
			int sign = normal[axial] > 0? 1: -1;
			VectorClear (normal);
			normal[axial] = sign;
		}

		if (numplanes >= MAXSIZE)
		{
			failed = true;
			continue;
		}
		VectorCopy (normal, planes[numplanes].normal);
		planes[numplanes].dist = DotProduct (p[1], normal);
		numplanes++;
	}

	// windings

	for (i = 0; i < numplanes; i++)
	{
		w[i] = new Winding (planes[i].normal, planes[i].dist);
		for (j = 0; j < numplanes; j++)
		{
			if (j == i)
			{
				continue;
			}
			vec3_t normal;
			vec_t dist;
			VectorSubtract (vec3_origin, planes[j].normal, normal);
			dist = -planes[j].dist;
			if (!w[i]->Chop (normal, dist))
			{
				failed = true;
				break;
			}
		}
	}

	// edges
	numedges = 0;
	for (i = 0; i < numplanes; i++)
	{
		for (e = 0; e < w[i]->m_NumPoints; e++)
		{
			hullbrushedge_t *edge;
			int found;
			if (numedges >= MAXSIZE)
			{
				failed = true;
				continue;
			}
			edge = &edges[numedges];
			VectorCopy (w[i]->m_Points[(e + 1) % w[i]->m_NumPoints], edge->vertexes[0]);
			VectorCopy (w[i]->m_Points[e], edge->vertexes[1]);
			VectorCopy (edge->vertexes[0], edge->point);
			VectorSubtract (edge->vertexes[1], edge->vertexes[0], edge->delta);
			if (VectorLength (edge->delta) < 1 - ON_EPSILON)
			{
				failed = true;
				continue;
			}
			VectorCopy (planes[i].normal, edge->normals[0]);
			found = 0;
			for (k = 0; k < numplanes; k++)
			{
				for (e2 = 0; e2 < w[k]->m_NumPoints; e2++)
				{
					if (VectorCompare (w[k]->m_Points[(e2 + 1) % w[k]->m_NumPoints], edge->vertexes[1]) &&
						VectorCompare (w[k]->m_Points[e2], edge->vertexes[0]))
					{
						found++;
						VectorCopy (planes[k].normal, edge->normals[1]);
						j = k;
					}
				}
			}
			if (found != 1)
			{
				failed = true;
				continue;
			}
			if (fabs (DotProduct (edge->vertexes[0], edge->normals[0]) - planes[i].dist) > NORMAL_EPSILON
				|| fabs (DotProduct (edge->vertexes[1], edge->normals[0]) - planes[i].dist) > NORMAL_EPSILON
				|| fabs (DotProduct (edge->vertexes[0], edge->normals[1]) - planes[j].dist) > NORMAL_EPSILON
				|| fabs (DotProduct (edge->vertexes[1], edge->normals[1]) - planes[j].dist) > NORMAL_EPSILON)
			{
				failed = true;
				continue;
			}
			if (j > i)
			{
				numedges++;
			}
		}
	}

	// vertexes
	numvertexes = 0;
	for (i = 0; i < numplanes; i++)
	{
		for (e = 0; e < w[i]->m_NumPoints; e++)
		{
			vec3_t v;
			VectorCopy (w[i]->m_Points[e], v);
			for (j = 0; j < numvertexes; j++)
			{
				if (VectorCompare (vertexes[j].point, v))
				{
					break;
				}
			}
			if (j < numvertexes)
			{
				continue;
			}
			if (numvertexes > MAXSIZE)
			{
				failed = true;
				continue;
			}

			VectorCopy (v, vertexes[numvertexes].point);
			numvertexes++;

			for (k = 0; k < numplanes; k++)
			{
				if (fabs (DotProduct (v, planes[k].normal) - planes[k].dist) < ON_EPSILON)
				{
					if (fabs (DotProduct (v, planes[k].normal) - planes[k].dist) > NORMAL_EPSILON)
					{
						failed = true;
					}
				}
			}
		}
	}

	// copy to hull brush

	if (!failed)
	{
		hb = (hullbrush_t *)malloc (sizeof (hullbrush_t));
		hlassume (hb != NULL, assume_NoMemory);

		hb->numfaces = numplanes;
		hb->faces = (hullbrushface_t *)malloc (hb->numfaces * sizeof (hullbrushface_t));
		hlassume (hb->faces != NULL, assume_NoMemory);
		for (i = 0; i < numplanes; i++)
		{
			hullbrushface_t *f = &hb->faces[i];
			VectorCopy (planes[i].normal, f->normal);
			VectorCopy (w[i]->m_Points[0], f->point);
			f->numvertexes = w[i]->m_NumPoints;
			f->vertexes = (vec3_t *)malloc (f->numvertexes * sizeof (vec3_t));
			hlassume (f->vertexes != NULL, assume_NoMemory);
			for (k = 0; k < w[i]->m_NumPoints; k++)
			{
				VectorCopy (w[i]->m_Points[k], f->vertexes[k]);
			}
		}

		hb->numedges = numedges;
		hb->edges = (hullbrushedge_t *)malloc (hb->numedges * sizeof (hullbrushedge_t));
		hlassume (hb->edges != NULL, assume_NoMemory);
		memcpy (hb->edges, edges, hb->numedges * sizeof (hullbrushedge_t));

		hb->numvertexes = numvertexes;
		hb->vertexes = (hullbrushvertex_t *)malloc (hb->numvertexes * sizeof (hullbrushvertex_t));
		hlassume (hb->vertexes != NULL, assume_NoMemory);
		memcpy (hb->vertexes, vertexes, hb->numvertexes * sizeof (hullbrushvertex_t));

		Developer (DEVELOPER_LEVEL_MESSAGE, "info_hullshape @ (%.0f,%.0f,%.0f): %d faces, %d edges, %d vertexes.\n", origin[0], origin[1], origin[2], hb->numfaces, hb->numedges, hb->numvertexes);
	}
	else
	{
		hb = NULL;
		Error ("Entity %i, Brush %i: invalid brush. This brush cannot be used for info_hullshape.",
				b->originalentitynum, b->originalbrushnum
				);
	}

	for (i = 0; i < numplanes; i++)
	{
		delete w[i];
	}

	return hb;
}

hullbrush_t *CopyHullBrush (const hullbrush_t *hb)
{
	hullbrush_t *hb2;
	hb2 = (hullbrush_t *)malloc (sizeof (hullbrush_t));
	hlassume (hb2 != NULL, assume_NoMemory);
	memcpy (hb2, hb, sizeof (hullbrush_t));
	hb2->faces = (hullbrushface_t *)malloc (hb->numfaces * sizeof (hullbrushface_t));
	hlassume (hb2->faces != NULL, assume_NoMemory);
	memcpy (hb2->faces, hb->faces, hb->numfaces * sizeof (hullbrushface_t));
	hb2->edges = (hullbrushedge_t *)malloc (hb->numedges * sizeof (hullbrushedge_t));
	hlassume (hb2->edges != NULL, assume_NoMemory);
	memcpy (hb2->edges, hb->edges, hb->numedges * sizeof (hullbrushedge_t));
	hb2->vertexes = (hullbrushvertex_t *)malloc (hb->numvertexes * sizeof (hullbrushvertex_t));
	hlassume (hb2->vertexes != NULL, assume_NoMemory);
	memcpy (hb2->vertexes, hb->vertexes, hb->numvertexes * sizeof (hullbrushvertex_t));
	for (int i = 0; i < hb->numfaces; i++)
	{
		hullbrushface_t *f2 = &hb2->faces[i];
		const hullbrushface_t *f = &hb->faces[i];
		f2->vertexes = (vec3_t *)malloc (f->numvertexes * sizeof (vec3_t));
		hlassume (f2->vertexes != NULL, assume_NoMemory);
		memcpy (f2->vertexes, f->vertexes, f->numvertexes * sizeof (vec3_t));
	}
	return hb2;
}

void DeleteHullBrush (hullbrush_t *hb)
{
	for (hullbrushface_t *hbf = hb->faces; hbf < hb->faces + hb->numfaces; hbf++)
	{
		if (hbf->vertexes)
		{
			free (hbf->vertexes);
		}
	}
	free (hb->faces);
	free (hb->edges);
	free (hb->vertexes);
	free (hb);
}

void InitDefaultHulls ()
{
	for (int h = 0; h < NUM_HULLS; h++)
	{
		hullshape_t *hs = &g_defaulthulls[h];
		hs->id = _strdup ("");
		hs->disabled = true;
		hs->numbrushes = 0;
		hs->brushes = (hullbrush_t **)malloc (0 * sizeof (hullbrush_t *));
		hlassume (hs->brushes != NULL, assume_NoMemory);
	}
}

void CreateHullShape (int entitynum, bool disabled, const char *id, int defaulthulls)
{
	entity_t *entity;
	hullshape_t *hs;

	entity = &g_entities[entitynum];
	if (!*ValueForKey (entity, "origin"))
	{
		Warning ("info_hullshape with no ORIGIN brush.");
	}
	if (g_numhullshapes >= MAX_HULLSHAPES)
	{
		Error ("Too many info_hullshape entities. Can not exceed %d.", MAX_HULLSHAPES);
	}
	hs = &g_hullshapes[g_numhullshapes];
	g_numhullshapes++;

	hs->id = _strdup (id);
	hs->disabled = disabled;
	hs->numbrushes = 0;
	hs->brushes = (hullbrush_t **)malloc (entity->numbrushes * sizeof (hullbrush_t *));
	for (int i = 0; i < entity->numbrushes; i++)
	{
		brush_t *b = &g_mapbrushes[entity->firstbrush + i];
		if (b->contents == CONTENTS_ORIGIN)
		{
			continue;
		}
		hs->brushes[hs->numbrushes] = CreateHullBrush (b);
		hs->numbrushes++;
	}
	if (hs->numbrushes >= 2)
	{
		brush_t *b = &g_mapbrushes[entity->firstbrush];
		Error ("Entity %i, Brush %i: Too many brushes in info_hullshape.",
			b->originalentitynum, b->originalbrushnum
			);
	}

	for (int h = 0; h < NUM_HULLS; h++)
	{
		if (defaulthulls & (1 << h))
		{
			hullshape_t *target = &g_defaulthulls[h];
			for (int i = 0; i < target->numbrushes; i++)
			{
				DeleteHullBrush (target->brushes[i]);
			}
			free (target->brushes);
			free (target->id);
			target->id = _strdup (hs->id);
			target->disabled = hs->disabled;
			target->numbrushes = hs->numbrushes;
			target->brushes = (hullbrush_t **)malloc (hs->numbrushes * sizeof (hullbrush_t *));
			hlassume (target->brushes != NULL, assume_NoMemory);
			for (int i = 0; i < hs->numbrushes; i++)
			{
				target->brushes[i] = CopyHullBrush (hs->brushes[i]);
			}
		}
	}
}
