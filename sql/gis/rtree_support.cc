// Copyright (c) 2017, Oracle and/or its affiliates. All rights reserved.
//
// This program is free software; you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software
// Foundation; version 2 of the License.
//
// This program is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
// details.
//
// You should have received a copy of the GNU General Public License along with
// this program; if not, write to the Free Software Foundation, 51 Franklin
// Street, Suite 500, Boston, MA 02110-1335 USA

/// @file
///
/// This file implements the set of functions that storage engines can call to
/// do geometrical operations.

#include "sql/gis/rtree_support.h"

#include <algorithm>  // std::min, std::max
#include <cmath>      // std::isfinite, std::isnan
#include <limits>

#include <boost/geometry.hpp>

#include "my_byteorder.h"  // doubleget, float8get
#include "my_inttypes.h"   // uchar
#include "sql/current_thd.h"
#include "sql/dd/cache/dictionary_client.h"
#include "sql/dd/types/spatial_reference_system.h"
#include "sql/gis/box.h"
#include "sql/gis/box_traits.h"
#include "sql/gis/covered_by_functor.h"
#include "sql/gis/equals_functor.h"
#include "sql/gis/geometries.h"
#include "sql/gis/geometries_cs.h"
#include "sql/gis/mbr_utils.h"
#include "sql/gis/srid.h"
#include "sql/gis/wkb_parser.h"
#include "sql/spatial.h"    // SRID_SIZE
#include "sql/sql_class.h"  // THD
#include "sql/srs_fetcher.h"
#include "template_utils.h"  // pointer_cast

namespace bg = boost::geometry;

dd::Spatial_reference_system* fetch_srs(gis::srid_t srid) {
  const dd::Spatial_reference_system* srs = nullptr;
  dd::cache::Dictionary_client::Auto_releaser m_releaser(
      current_thd->dd_client());
  Srs_fetcher fetcher(current_thd);
  if (srid != 0 && fetcher.acquire(srid, &srs)) return nullptr;

  if (srs)
    return srs->clone();
  else
    return nullptr;
}

bool mbr_contain_cmp(const dd::Spatial_reference_system* srs, rtr_mbr_t* a,
                     rtr_mbr_t* b) {
  DBUG_ASSERT(a->xmin <= a->xmax && a->ymin <= a->ymax);
  DBUG_ASSERT(b->xmin <= b->xmax && b->ymin <= b->ymax);

  bool result = false;
  try {
    gis::Covered_by covered_by(srs ? srs->semi_major_axis() : 0.0,
                               srs ? srs->semi_minor_axis() : 0.0);
    if (srs == nullptr || srs->is_cartesian()) {
      gis::Cartesian_box a_box(gis::Cartesian_point(a->xmin, a->ymin),
                               gis::Cartesian_point(a->xmax, a->ymax));
      gis::Cartesian_box b_box(gis::Cartesian_point(b->xmin, b->ymin),
                               gis::Cartesian_point(b->xmax, b->ymax));
      result = covered_by(&b_box, &a_box);
    } else {
      DBUG_ASSERT(srs->is_geographic());
      gis::Geographic_box a_box(
          gis::Geographic_point(srs->to_radians(a->xmin),
                                srs->to_radians(a->ymin)),
          gis::Geographic_point(srs->to_radians(a->xmax),
                                srs->to_radians(a->ymax)));
      gis::Geographic_box b_box(
          gis::Geographic_point(srs->to_radians(b->xmin),
                                srs->to_radians(b->ymin)),
          gis::Geographic_point(srs->to_radians(b->xmax),
                                srs->to_radians(b->ymax)));
      result = covered_by(&b_box, &a_box);
    }
  } catch (...) {
    DBUG_ASSERT(false); /* purecov: inspected */
  }

  return result;
}

bool mbr_equal_cmp(const dd::Spatial_reference_system* srs, rtr_mbr_t* a,
                   rtr_mbr_t* b) {
  DBUG_ASSERT(a->xmin <= a->xmax && a->ymin <= a->ymax);
  DBUG_ASSERT(b->xmin <= b->xmax && b->ymin <= b->ymax);

  bool result = false;
  try {
    gis::Equals equals(srs ? srs->semi_major_axis() : 0.0,
                       srs ? srs->semi_minor_axis() : 0.0);
    if (srs == nullptr || srs->is_cartesian()) {
      gis::Cartesian_box a_box(gis::Cartesian_point(a->xmin, a->ymin),
                               gis::Cartesian_point(a->xmax, a->ymax));
      gis::Cartesian_box b_box(gis::Cartesian_point(b->xmin, b->ymin),
                               gis::Cartesian_point(b->xmax, b->ymax));
      result = equals(&a_box, &b_box);
    } else {
      DBUG_ASSERT(srs->is_geographic());
      gis::Geographic_box a_box(
          gis::Geographic_point(srs->to_radians(a->xmin),
                                srs->to_radians(a->ymin)),
          gis::Geographic_point(srs->to_radians(a->xmax),
                                srs->to_radians(a->ymax)));
      gis::Geographic_box b_box(
          gis::Geographic_point(srs->to_radians(b->xmin),
                                srs->to_radians(b->ymin)),
          gis::Geographic_point(srs->to_radians(b->xmax),
                                srs->to_radians(b->ymax)));
      result = equals(&a_box, &b_box);
    }
  } catch (...) {
    DBUG_ASSERT(false); /* purecov: inspected */
  }

  return result;
}

bool mbr_intersect_cmp(const dd::Spatial_reference_system* srs, rtr_mbr_t* a,
                       rtr_mbr_t* b) {
  // This assertion contains the old return value of the function. Given a valid
  // box, it should always be true.
  DBUG_ASSERT((b->xmin <= a->xmax || b->xmax >= a->xmin) &&
              (b->ymin <= a->ymax || b->ymax >= a->ymin));
  return true;
}

bool mbr_disjoint_cmp(const dd::Spatial_reference_system* srs, rtr_mbr_t* a,
                      rtr_mbr_t* b) {
  return !mbr_intersect_cmp(srs, a, b);
}

bool mbr_within_cmp(const dd::Spatial_reference_system* srs, rtr_mbr_t* a,
                    rtr_mbr_t* b) {
  bool result = false;
  try {
    // If min and max coordinates have been reversed, InnoDB expects the result
    // to be inverse too. But not if a and b have the exact same coordinates.
    bool invert = false;
    if (a->xmin > a->xmax && a->ymin > a->ymax &&
        !(a->xmin == b->xmin && a->ymin == b->ymin && a->xmax == b->xmax &&
          a->ymax == b->ymax)) {
      invert = true;
    }

    // Correct the min and max corners to generate proper boxes.
    double a_xmin = std::min(a->xmin, a->xmax);
    double a_ymin = std::min(a->ymin, a->ymax);
    double a_xmax = std::max(a->xmin, a->xmax);
    double a_ymax = std::max(a->ymin, a->ymax);
    double b_xmin = std::min(b->xmin, b->xmax);
    double b_ymin = std::min(b->ymin, b->ymax);
    double b_xmax = std::max(b->xmin, b->xmax);
    double b_ymax = std::max(b->ymin, b->ymax);

    gis::Covered_by covered_by(srs ? srs->semi_major_axis() : 0.0,
                               srs ? srs->semi_minor_axis() : 0.0);
    if (srs == nullptr || srs->is_cartesian()) {
      gis::Cartesian_box a_box(gis::Cartesian_point(a_xmin, a_ymin),
                               gis::Cartesian_point(a_xmax, a_ymax));
      gis::Cartesian_box b_box(gis::Cartesian_point(b_xmin, b_ymin),
                               gis::Cartesian_point(b_xmax, b_ymax));
      result = covered_by(&a_box, &b_box);
    } else {
      DBUG_ASSERT(srs->is_geographic());
      gis::Geographic_box a_box(gis::Geographic_point(srs->to_radians(a_xmin),
                                                      srs->to_radians(a_ymin)),
                                gis::Geographic_point(srs->to_radians(a_xmax),
                                                      srs->to_radians(a_ymax)));
      gis::Geographic_box b_box(gis::Geographic_point(srs->to_radians(b_xmin),
                                                      srs->to_radians(b_ymin)),
                                gis::Geographic_point(srs->to_radians(b_xmax),
                                                      srs->to_radians(b_ymax)));
      result = covered_by(&a_box, &b_box);
    }
    if (invert) result = !result;
  } catch (...) {
    DBUG_ASSERT(false); /* purecov: inspected */
  }

  return result;
}

void mbr_join(const dd::Spatial_reference_system* srs, double* a,
              const double* b, int n_dim) {
  DBUG_ASSERT(n_dim == 2);

  try {
    if (srs == nullptr || srs->is_cartesian()) {
      gis::Cartesian_box a_box(gis::Cartesian_point(a[0], a[2]),
                               gis::Cartesian_point(a[1], a[3]));
      gis::Cartesian_box b_box(gis::Cartesian_point(b[0], b[2]),
                               gis::Cartesian_point(b[1], b[3]));
      bg::expand(a_box, b_box);
      a[0] = a_box.min_corner().x();
      a[1] = a_box.max_corner().x();
      a[2] = a_box.min_corner().y();
      a[3] = a_box.max_corner().y();
    } else {
      DBUG_ASSERT(srs->is_geographic());
      gis::Geographic_box a_box(
          gis::Geographic_point(srs->to_radians(a[0]), srs->to_radians(a[2])),
          gis::Geographic_point(srs->to_radians(a[1]), srs->to_radians(a[3])));
      gis::Geographic_box b_box(
          gis::Geographic_point(srs->to_radians(b[0]), srs->to_radians(b[2])),
          gis::Geographic_point(srs->to_radians(b[1]), srs->to_radians(b[3])));
      bg::expand(a_box, b_box);
      a[0] = srs->from_radians(a_box.min_corner().x());
      a[1] = srs->from_radians(a_box.max_corner().x());
      a[2] = srs->from_radians(a_box.min_corner().y());
      a[3] = srs->from_radians(a_box.max_corner().y());
    }
  } catch (...) {
    DBUG_ASSERT(false); /* purecov: inspected */
  }
}

double mbr_join_area(const dd::Spatial_reference_system* srs, const double* a,
                     const double* b, int n_dim) {
  DBUG_ASSERT(n_dim == 2);

  double area = 0.0;
  try {
    if (srs == nullptr || srs->is_cartesian()) {
      gis::Cartesian_box a_box(gis::Cartesian_point(a[0], a[2]),
                               gis::Cartesian_point(a[1], a[3]));
      gis::Cartesian_box b_box(gis::Cartesian_point(b[0], b[2]),
                               gis::Cartesian_point(b[1], b[3]));
      bg::expand(a_box, b_box);
      area = bg::area(a_box);
    } else {
      DBUG_ASSERT(srs->is_geographic());
      gis::Geographic_box a_box(
          gis::Geographic_point(srs->to_radians(a[0]), srs->to_radians(a[2])),
          gis::Geographic_point(srs->to_radians(a[1]), srs->to_radians(a[3])));
      gis::Geographic_box b_box(
          gis::Geographic_point(srs->to_radians(b[0]), srs->to_radians(b[2])),
          gis::Geographic_point(srs->to_radians(b[1]), srs->to_radians(b[3])));
      bg::expand(a_box, b_box);
      area = bg::area(
          a_box, bg::strategy::area::geographic<
                     gis::Geographic_point, bg::strategy::andoyer,
                     bg::strategy::default_order<bg::strategy::andoyer>::value,
                     bg::srs::spheroid<double>>(bg::srs::spheroid<double>(
                     srs->semi_major_axis(), srs->semi_minor_axis())));
    }
  } catch (...) {
    DBUG_ASSERT(false); /* purecov: inspected */
  }

  if (!std::isfinite(area)) area = std::numeric_limits<double>::max();
  return area;
}

double compute_area(const dd::Spatial_reference_system* srs, const double* a,
                    int n_dim) {
  DBUG_ASSERT(n_dim == 2);

  double area = 0.0;
  try {
    if (srs == nullptr || srs->is_cartesian()) {
      gis::Cartesian_box a_box(gis::Cartesian_point(a[0], a[2]),
                               gis::Cartesian_point(a[1], a[3]));
      area = bg::area(a_box);
    } else {
      DBUG_ASSERT(srs->is_geographic());
      gis::Geographic_box a_box(
          gis::Geographic_point(srs->to_radians(a[0]), srs->to_radians(a[2])),
          gis::Geographic_point(srs->to_radians(a[1]), srs->to_radians(a[3])));
      area = bg::area(
          a_box, bg::strategy::area::geographic<
                     gis::Geographic_point, bg::strategy::andoyer,
                     bg::strategy::default_order<bg::strategy::andoyer>::value,
                     bg::srs::spheroid<double>>(bg::srs::spheroid<double>(
                     srs->semi_major_axis(), srs->semi_minor_axis())));
    }
  } catch (...) {
    DBUG_ASSERT(false); /* purecov: inspected */
  }

  return area;
}

int get_mbr_from_store(const dd::Spatial_reference_system* srs, uchar* store,
                       uint size, uint n_dims, double* mbr, gis::srid_t* srid) {
  DBUG_ASSERT(n_dims == 2);
  // The SRS should match the SRID of the geometry, with one exception: For
  // backwards compatibility it is allowed to create indexes with mixed
  // SRIDs. Although these indexes can never be used to optimize queries, the
  // user is allowed to create them. These indexes will call get_mbr_from_store
  // with srs == nullptr. There is, unfortunately, no way to differentiate mixed
  // SRID indexes from SRID 0 indexes here, so the assertion is not perfect.
  DBUG_ASSERT(srs == nullptr || (srs->id() == uint4korr(store)));

  if (srid != nullptr) *srid = uint4korr(store);

  try {
    std::unique_ptr<gis::Geometry> g =
        gis::parse_wkb(srs, pointer_cast<char*>(store) + sizeof(gis::srid_t),
                       size - sizeof(gis::srid_t), true);
    if (g.get() == nullptr) {
      return -1; /* purecov: inspected */
    }
    if (srs == nullptr || srs->is_cartesian()) {
      gis::Cartesian_box box;
      gis::box_envelope(g.get(), srs, &box);
      mbr[0] = box.min_corner().x();
      mbr[1] = box.max_corner().x();
      mbr[2] = box.min_corner().y();
      mbr[3] = box.max_corner().y();
    } else {
      DBUG_ASSERT(srs->is_geographic());
      gis::Geographic_box box;
      gis::box_envelope(g.get(), srs, &box);
      mbr[0] = srs->from_radians(box.min_corner().x());
      mbr[1] = srs->from_radians(box.max_corner().x());
      mbr[2] = srs->from_radians(box.min_corner().y());
      mbr[3] = srs->from_radians(box.max_corner().y());
    }
  } catch (...) {
    DBUG_ASSERT(false); /* purecov: inspected */
    return -1;
  }

  if (std::isnan(mbr[0])) {
    /* purecov: begin inspected */
    DBUG_ASSERT(std::isnan(mbr[1]) && std::isnan(mbr[2]) && std::isnan(mbr[3]));
    // The geometry is empty, so there is no bounding box. Return a box that
    // covers the entire domain.
    mbr[0] = std::numeric_limits<double>::lowest();
    mbr[1] = std::numeric_limits<double>::max();
    mbr[2] = std::numeric_limits<double>::lowest();
    mbr[3] = std::numeric_limits<double>::max();
    /* purecov: end inspected */
  }

  // xmin <= xmax && ymin <= ymax
  DBUG_ASSERT(mbr[0] <= mbr[1] && mbr[2] <= mbr[3]);

  return 0;
}

double rtree_area_increase(const dd::Spatial_reference_system* srs,
                           const uchar* mbr_a, const uchar* mbr_b, int mbr_len,
                           double* ab_area) {
  double a_area = 1.0;
  double loc_ab_area = 1.0;
  double amin, amax, bmin, bmax;
  int key_len;
  int keyseg_len;
  double data_round = 1.0;
  /*
    Since the mbr could be a point or a linestring, in this case, area of mbr is
    0. So, we define this macro for calculating the area increasing when we need
    to
    enlarge the mbr.
  */
  double line_mbr_weights = 0.001;

  keyseg_len = 2 * sizeof(double);

  for (key_len = mbr_len; key_len > 0; key_len -= keyseg_len) {
    double area;

#ifdef WORDS_BIGENDIAN
    float8get(&amin, mbr_a);
    float8get(&bmin, mbr_b);
    float8get(&amax, mbr_a + sizeof(double));
    float8get(&bmax, mbr_b + sizeof(double));
#else
    doubleget(&amin, mbr_a);
    doubleget(&bmin, mbr_b);
    doubleget(&amax, mbr_a + sizeof(double));
    doubleget(&bmax, mbr_b + sizeof(double));
#endif

    area = amax - amin;
    if (area == 0)
      a_area *= line_mbr_weights;
    else
      a_area *= area;

    area = (double)std::max(amax, bmax) - (double)std::min(amin, bmin);

    if (area == 0)
      loc_ab_area *= line_mbr_weights;
    else
      loc_ab_area *= area;

    /* Value of amax or bmin can be so large that small difference
    are ignored. For example: 3.2884281489988079e+284 - 100 =
    3.2884281489988079e+284. This results some area difference
    are not detected */
    if (loc_ab_area == a_area) {
      if (bmin < amin || bmax > amax)
        data_round *= ((double)std::max(amax, bmax) - amax +
                       (amin - (double)std::min(amin, bmin)));
      else
        data_round *= area;
    }

    mbr_a += keyseg_len;
    mbr_b += keyseg_len;
  }

  *ab_area = loc_ab_area;

  if (loc_ab_area == a_area && data_round != 1.0) return (data_round);

  return (loc_ab_area - a_area);
}

double rtree_area_overlapping(const dd::Spatial_reference_system* srs,
                              const uchar* mbr_a, const uchar* mbr_b,
                              int mbr_len) {
  double area = 1.0;
  double amin;
  double amax;
  double bmin;
  double bmax;
  int key_len;
  int keyseg_len;

  keyseg_len = 2 * sizeof(double);

  for (key_len = mbr_len; key_len > 0; key_len -= keyseg_len) {
#ifdef WORDS_BIGENDIAN
    float8get(&amin, mbr_a);
    float8get(&bmin, mbr_b);
    float8get(&amax, mbr_a + sizeof(double));
    float8get(&bmax, mbr_b + sizeof(double));
#else
    doubleget(&amin, mbr_a);
    doubleget(&bmin, mbr_b);
    doubleget(&amax, mbr_a + sizeof(double));
    doubleget(&bmax, mbr_b + sizeof(double));
#endif

    amin = std::max(amin, bmin);
    amax = std::min(amax, bmax);

    if (amin > amax)
      return (0);
    else
      area *= (amax - amin);

    mbr_a += keyseg_len;
    mbr_b += keyseg_len;
  }

  return (area);
}
