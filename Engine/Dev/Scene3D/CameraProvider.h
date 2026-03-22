/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
 * (C) 2013-2018 INRIA and Alexandre Gauthier-Foichat
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Natron is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Natron.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
 * ***** END LICENSE BLOCK ***** */

#ifndef NATRON_ENGINE_CAMERAPROVIDER_H
#define NATRON_ENGINE_CAMERAPROVIDER_H

/**
 * @brief Interface for any node that can provide camera data.
 *
 * Implemented by Camera3DNode and ReadAlembicCamera.
 * Used by ScanlineRender, Project3D, etc. to get camera matrices
 * from their cam input without caring which type of camera node is connected.
 */
class CameraProvider
{
public:
    virtual ~CameraProvider() {}

    virtual void getCameraPosition(double time,
                                   double& tx, double& ty, double& tz,
                                   double& rx, double& ry, double& rz) const = 0;

    virtual double getCameraFocalLength(double time) const = 0;
    virtual double getCameraHAperture(double time) const = 0;
    virtual double getCameraVAperture(double time) const { (void)time; return 18.672; }
    virtual double getCameraNear(double time) const { (void)time; return 0.1; }
    virtual double getCameraFar(double time) const { (void)time; return 10000.0; }
};

#endif // NATRON_ENGINE_CAMERAPROVIDER_H
