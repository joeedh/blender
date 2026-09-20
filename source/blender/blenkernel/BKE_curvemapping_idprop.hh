/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <string>

#include "BLI_string_ref.hh"

namespace blender {

struct CurveMapping;
struct IDProperty;

/** Validate an owned scalar curve definition without modifying its storage. */
bool BKE_curvemapping_idprop_validate(const IDProperty &definition, std::string &error);

/** Materialize a validated scalar definition; caller frees with BKE_curvemapping_free. */
CurveMapping *BKE_curvemapping_from_idprop(const IDProperty &definition, std::string &error);

/** Encode scalar authoring data, preserving unknown top-level keys from previous. */
IDProperty *BKE_curvemapping_to_idprop(const CurveMapping &mapping,
                                       StringRef name,
                                       const IDProperty *previous,
                                       std::string &error);

}  // namespace blender
