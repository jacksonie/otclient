/*
 * Copyright (c) 2010-2022 OTClient <https://github.com/edubart/otclient>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "mapview.h"

#include "animatedtext.h"
#include "creature.h"
#include "game.h"
#include "lightview.h"
#include "map.h"
#include "missile.h"
#include "shadermanager.h"
#include "statictext.h"
#include "tile.h"

#include <framework/core/application.h>
#include <framework/core/eventdispatcher.h>
#include <framework/core/resourcemanager.h>
#include <framework/graphics/drawpool.h>
#include <framework/graphics/graphics.h>

#include "framework/graphics/texturemanager.h"

#include <framework/platform/platformwindow.h>

MapView::MapView()
{
    auto* mapPool = g_drawPool.get<PoolFramed>(PoolType::MAP);

    mapPool->onBeforeDraw([&] {
        const Position cameraPosition = getCameraPosition();

        float fadeOpacity = 1.f;
        if (!m_shaderSwitchDone && m_fadeOutTime > 0) {
            fadeOpacity = 1.f - (m_fadeTimer.timeElapsed() / m_fadeOutTime);
            if (fadeOpacity < 0.f) {
                m_shader = m_nextShader;
                m_nextShader = nullptr;
                m_shaderSwitchDone = true;
                m_fadeTimer.restart();
            }
        }

        if (m_shaderSwitchDone && m_shader && m_fadeInTime > 0)
            fadeOpacity = std::min<float>(m_fadeTimer.timeElapsed() / m_fadeInTime, 1.f);

        if (m_shader && g_painter->hasShaders() && g_graphics.shouldUseShaders()) {
            auto framebufferRect = Rect(0, 0, m_drawDimension * m_tileSize);
            const Point center = m_rectCache.srcRect.center();
            const Point globalCoord = Point(cameraPosition.x - m_drawDimension.width() / 2, -(cameraPosition.y - m_drawDimension.height() / 2)) * m_tileSize;
            m_shader->bind();
            m_shader->setUniformValue(ShaderManager::MAP_CENTER_COORD, center.x / static_cast<float>(m_rectDimension.width()), 1.f - center.y / static_cast<float>(m_rectDimension.height()));
            m_shader->setUniformValue(ShaderManager::MAP_GLOBAL_COORD, globalCoord.x / static_cast<float>(m_rectDimension.height()), globalCoord.y / static_cast<float>(m_rectDimension.height()));
            m_shader->setUniformValue(ShaderManager::MAP_ZOOM, m_scaleFactor);

            Point last = transformPositionTo2D(cameraPosition, m_shader->getPosition());
            //Reverse vertical axis.
            last.y = -last.y;

            m_shader->setUniformValue(ShaderManager::MAP_WALKOFFSET, last.x / static_cast<float>(m_rectDimension.width()), last.y / static_cast<float>(m_rectDimension.height()));

            g_painter->setShaderProgram(m_shader);
        }

        g_painter->setOpacity(fadeOpacity);
    });

    mapPool->onAfterDraw([&] {
        g_painter->resetShaderProgram();
        g_painter->resetOpacity();
    });

    m_shader = g_shaders.getDefaultMapShader();

    setVisibleDimension(Size(15, 11));
}

MapView::~MapView()
{
#ifndef NDEBUG
    assert(!g_app.isTerminated());
#endif
    m_lightView = nullptr;
}

void MapView::draw(const Rect& rect)
{
    // update visible tiles cache when needed
    if (m_refreshVisibleTiles)
        updateVisibleThings();

    if (m_rectCache.rect != rect) {
        m_rectCache.rect = rect;
        m_rectCache.srcRect = calcFramebufferSource(rect.size());
        m_rectCache.drawOffset = m_rectCache.srcRect.topLeft();
        m_rectCache.horizontalStretchFactor = rect.width() / static_cast<float>(m_rectCache.srcRect.width());
        m_rectCache.verticalStretchFactor = rect.height() / static_cast<float>(m_rectCache.srcRect.height());
    }

    if (canFloorFade()) {
        const float fadeLevel = getFadeLevel(m_cachedFirstVisibleFloor);
        if (m_lastFadeLevel != fadeLevel && fadeLevel == 1.f) {
            onFadeInFinished();
            m_lastFadeLevel = fadeLevel;
        }
    }

    drawFloor();

    // this could happen if the player position is not known yet
    if (!getCameraPosition().isValid()) {
        return;
    }

    drawCreatureInformation();
    if (m_drawLights) m_lightView->draw(rect, m_rectCache.srcRect);
    drawText();
}

void MapView::drawFloor()
{
    g_drawPool.use(PoolType::MAP, m_rectCache.rect, m_rectCache.srcRect, Color::black);
    {
        const Position cameraPosition = getCameraPosition();
        const auto& lightView = m_drawLights ? m_lightView.get() : nullptr;

        for (int_fast8_t z = m_floorMax; z >= m_floorMin; --z) {
            if (canFloorFade()) {
                const float fading = getFadeLevel(z);
                if (fading == 0) break;
                if (fading < 0.99)
                    g_drawPool.setOpacity(fading);
            }

            Position _camera = cameraPosition;
            bool alwaysTransparent = m_floorViewMode == ALWAYS_WITH_TRANSPARENCY && z < m_cachedFirstVisibleFloor&& _camera.coveredUp(cameraPosition.z - z);

            const auto& map = m_cachedVisibleTiles[z];

            for (const auto& tile : map.grounds) {
                if (!tile->canRender(m_drawViewportEdge, cameraPosition, m_viewport, lightView))
                    continue;

                if (alwaysTransparent)
                    g_drawPool.setOpacity(tile->getPosition().isInRange(_camera, TRANSPARENT_FLOOR_VIEW_RANGE, TRANSPARENT_FLOOR_VIEW_RANGE, true) ? .16 : .7);

                tile->drawGround(transformPositionTo2D(tile->getPosition(), cameraPosition), m_scaleFactor, lightView);

                if (alwaysTransparent)
                    g_drawPool.resetOpacity();
            }

            for (const auto& tile : map.surfaces) {
                if (!tile->canRender(m_drawViewportEdge, cameraPosition, m_viewport, lightView))
                    continue;

                if (alwaysTransparent)
                    g_drawPool.setOpacity(tile->getPosition().isInRange(_camera, TRANSPARENT_FLOOR_VIEW_RANGE, TRANSPARENT_FLOOR_VIEW_RANGE, true) ? .16 : .7);

                tile->drawSurface(transformPositionTo2D(tile->getPosition(), cameraPosition), m_scaleFactor, lightView);

                if (alwaysTransparent)
                    g_drawPool.resetOpacity();
            }

            for (const auto& tile : map.effects) {
                const auto& dest = transformPositionTo2D(tile->getPosition(), cameraPosition);
                for (const auto& effect : tile->getEffects()) {
                    effect->drawEffect(dest, m_scaleFactor, lightView);
                }
            }
            for (const MissilePtr& missile : g_map.getFloorMissiles(z))
                missile->drawMissile(transformPositionTo2D(missile->getPosition(), cameraPosition), m_scaleFactor, lightView);

            if (m_shadowFloorIntensity > 0 && z == cameraPosition.z + 1) {
                g_drawPool.addFilledRect(m_rectDimension, Color::black);
                g_drawPool.setOpacity(m_shadowFloorIntensity, g_drawPool.size());
            }

            if (isDrawingLights()) {
                const int8_t nextFloor = z - 1;
                if (nextFloor >= m_floorMin) {
                    const float fadeLevel = canFloorFade() ? getFadeLevel(nextFloor) : 1.f;
                    if (fadeLevel == 0.f)
                        continue;

                    _camera = cameraPosition;
                    alwaysTransparent = m_floorViewMode == ALWAYS_WITH_TRANSPARENCY && nextFloor < cameraPosition.z&& _camera.coveredUp(cameraPosition.z - nextFloor);

                    for (const auto& tile : m_cachedVisibleTiles[nextFloor].shades) {
                        if (alwaysTransparent && tile->getPosition().isInRange(_camera, TRANSPARENT_FLOOR_VIEW_RANGE, TRANSPARENT_FLOOR_VIEW_RANGE, true))
                            continue;

                        auto pos2D = transformPositionTo2D(tile->getPosition(), cameraPosition);
                        lightView->addShade(pos2D, fadeLevel);
                    }
                }
            }

            if (canFloorFade())
                g_drawPool.resetOpacity();

            g_drawPool.flush();
        }

        if (m_rectCache.rect.contains(g_window.getMousePosition())) {
            if (m_crosshairTexture) {
                const Point& point = transformPositionTo2D(m_mousePosition, cameraPosition);
                const auto crosshairRect = Rect(point, m_tileSize, m_tileSize);
                g_drawPool.addTexturedRect(crosshairRect, m_crosshairTexture);
            }
        } else if (m_lastHighlightTile) {
            m_mousePosition = {}; // Invalidate mousePosition
            m_lastHighlightTile->unselect();
            m_lastHighlightTile = nullptr;
        }
    }
}

void MapView::drawCreatureInformation()
{
    if (!m_drawNames && !m_drawHealthBars && !m_drawManaBar) return;

    const Position cameraPosition = getCameraPosition();

    uint32_t flags = 0;
    if (m_drawNames) { flags = Otc::DrawNames; }
    if (m_drawHealthBars) { flags |= Otc::DrawBars; }
    if (m_drawManaBar) { flags |= Otc::DrawManaBar; }

    g_drawPool.use(PoolType::CREATURE_INFORMATION);
    for (const auto& creature : m_visibleCreatures) {
        if (creature->isDead() || !creature->canBeSeen())
            continue;

        const auto& tile = creature->getTile();
        if (!tile) continue;

        bool useGray = tile->isCovered(m_cachedFirstVisibleFloor);
        if (useGray && m_floorViewMode == ALWAYS_WITH_TRANSPARENCY) {
            useGray = !tile->getPosition().isInRange(cameraPosition, TRANSPARENT_FLOOR_VIEW_RANGE, TRANSPARENT_FLOOR_VIEW_RANGE, true);
        }

        creature->drawInformation(m_rectCache.rect,
                                  transformPositionTo2D(creature->getPosition(), cameraPosition),
                                  m_scaleFactor, m_rectCache.drawOffset, useGray,
                                  m_rectCache.horizontalStretchFactor, m_rectCache.verticalStretchFactor, flags);
    }
}

void MapView::drawText()
{
    if (!m_drawTexts || (g_map.getStaticTexts().empty() && g_map.getAnimatedTexts().empty())) return;

    const Position cameraPosition = getCameraPosition();

    g_drawPool.use(PoolType::TEXT);
    for (const StaticTextPtr& staticText : g_map.getStaticTexts()) {
        if (staticText->getMessageMode() == Otc::MessageNone) continue;

        const Position pos = staticText->getPosition();

        if (pos.z != cameraPosition.z)
            continue;

        Point p = transformPositionTo2D(pos, cameraPosition) - m_rectCache.drawOffset;
        p.x *= m_rectCache.horizontalStretchFactor;
        p.y *= m_rectCache.verticalStretchFactor;
        p += m_rectCache.rect.topLeft();
        staticText->drawText(p, m_rectCache.rect);
    }

    for (const AnimatedTextPtr& animatedText : g_map.getAnimatedTexts()) {
        const Position pos = animatedText->getPosition();

        if (pos.z != cameraPosition.z)
            continue;

        Point p = transformPositionTo2D(pos, cameraPosition) - m_rectCache.drawOffset;
        p.x *= m_rectCache.horizontalStretchFactor;
        p.y *= m_rectCache.verticalStretchFactor;
        p += m_rectCache.rect.topLeft();

        animatedText->drawText(p, m_rectCache.rect);
    }
}

void MapView::updateVisibleThings()
{
    // there is no tile to render on invalid positions
    const Position cameraPosition = getCameraPosition();
    if (!cameraPosition.isValid())
        return;

    // clear current visible tiles cache
    do {
        m_cachedVisibleTiles[m_floorMin].clear();
    } while (++m_floorMin <= m_floorMax);

    if (m_refreshVisibleCreatures) {
        m_visibleCreatures.clear();
    }

    if (m_floorViewMode == LOCKED) {
        m_lockedFirstVisibleFloor = cameraPosition.z;
    } else {
        m_lockedFirstVisibleFloor = -1;
    }

    const uint8_t prevFirstVisibleFloor = m_cachedFirstVisibleFloor;
    if (m_lastCameraPosition != cameraPosition) {
        if (m_mousePosition.isValid()) {
            const Otc::Direction direction = m_lastCameraPosition.getDirectionFromPosition(cameraPosition);
            m_mousePosition = m_mousePosition.translatedToDirection(direction);

            if (cameraPosition.z != m_lastCameraPosition.z) {
                m_mousePosition.z += cameraPosition.z - m_lastCameraPosition.z;
                m_mousePosition = m_mousePosition.translatedToDirection(direction); // Two steps
            }

            onMouseMove(m_mousePosition, true);
        }

        if (m_lastCameraPosition.z != cameraPosition.z) {
            onFloorChange(cameraPosition.z, m_lastCameraPosition.z);
        }

        const uint8_t cachedFirstVisibleFloor = calcFirstVisibleFloor(m_floorViewMode != ALWAYS);
        uint8_t cachedLastVisibleFloor = calcLastVisibleFloor();

        assert(cachedFirstVisibleFloor <= MAX_Z && cachedLastVisibleFloor <= MAX_Z);

        if (cachedLastVisibleFloor < cachedFirstVisibleFloor)
            cachedLastVisibleFloor = cachedFirstVisibleFloor;

        m_cachedFirstVisibleFloor = cachedFirstVisibleFloor;
        m_cachedLastVisibleFloor = cachedLastVisibleFloor;

        m_floorMin = m_floorMax = cameraPosition.z;
    }

    uint8_t cachedFirstVisibleFloor = m_cachedFirstVisibleFloor;
    if (m_floorViewMode == ALWAYS_WITH_TRANSPARENCY || canFloorFade()) {
        cachedFirstVisibleFloor = calcFirstVisibleFloor(false);
    }

    // Fading System by Kondra https://github.com/OTCv8/otclientv8
    if (!m_lastCameraPosition.isValid() || m_lastCameraPosition.z != cameraPosition.z || m_lastCameraPosition.distance(cameraPosition) >= 3) {
        for (int iz = m_cachedLastVisibleFloor; iz >= cachedFirstVisibleFloor; --iz) {
            m_fadingFloorTimers[iz].restart(m_floorFading * 1000);
        }
    } else if (prevFirstVisibleFloor < m_cachedFirstVisibleFloor) { // hiding new floor
        for (int iz = prevFirstVisibleFloor; iz < m_cachedFirstVisibleFloor; ++iz) {
            const int shift = std::max<int>(0, m_floorFading - m_fadingFloorTimers[iz].elapsed_millis());
            m_fadingFloorTimers[iz].restart(shift * 1000);
        }
    } else if (prevFirstVisibleFloor > m_cachedFirstVisibleFloor) { // showing floor
        m_lastFadeLevel = 0.f;
        for (int iz = m_cachedFirstVisibleFloor; iz < prevFirstVisibleFloor; ++iz) {
            const int shift = std::max<int>(0, m_floorFading - m_fadingFloorTimers[iz].elapsed_millis());
            m_fadingFloorTimers[iz].restart(shift * 1000);
        }
    }

    m_lastCameraPosition = cameraPosition;

    // cache visible tiles in draw order
    // draw from last floor (the lower) to first floor (the higher)
    const uint32_t numDiagonals = m_drawDimension.width() + m_drawDimension.height() - 1;
    for (int_fast32_t iz = m_cachedLastVisibleFloor; iz >= cachedFirstVisibleFloor; --iz) {
        auto& floor = m_cachedVisibleTiles[iz];

        // loop through / diagonals beginning at top left and going to top right
        for (uint_fast32_t diagonal = 0; diagonal < numDiagonals; ++diagonal) {
            // loop current diagonal tiles
            const uint32_t advance = std::max<uint32_t>(diagonal - m_drawDimension.height(), 0);
            for (int iy = diagonal - advance, ix = advance; iy >= 0 && ix < m_drawDimension.width(); --iy, ++ix) {
                // position on current floor
                //TODO: check position limits
                Position tilePos = cameraPosition.translated(ix - m_virtualCenterOffset.x, iy - m_virtualCenterOffset.y);
                // adjust tilePos to the wanted floor
                tilePos.coveredUp(cameraPosition.z - iz);
                if (const TilePtr& tile = g_map.getTile(tilePos)) {
                    // skip tiles that have nothing
                    if (!tile->isDrawable())
                        continue;

                    if (m_refreshVisibleCreatures && isInRange(tilePos)) {
                        const auto& tileCreatures = tile->getCreatures();
                        if (!tileCreatures.empty()) {
                            m_visibleCreatures.insert(m_visibleCreatures.end(), tileCreatures.rbegin(), tileCreatures.rend());
                        }
                    }

                    bool addTile = true;

                    if (isDrawingLights() && tile->canShade(this))
                        floor.shades.push_back(tile);

                    if (addTile) {
                        if (tile->hasGround())
                            floor.grounds.push_back(tile);

                        if (tile->hasSurface())
                            floor.surfaces.push_back(tile);

                        if (g_app.isDrawingEffectsOnTop() && tile->hasEffect())
                            floor.effects.push_back(tile);
                    }

                    if (addTile || !floor.shades.empty()) {
                        if (iz < m_floorMin)
                            m_floorMin = iz;
                        else if (iz > m_floorMax)
                            m_floorMax = iz;
                    }
                }
            }
        }
    }

    m_refreshVisibleCreatures = false;
    m_refreshVisibleTiles = false;
}

void MapView::updateGeometry(const Size& visibleDimension)
{
    const uint8_t tileSize = SPRITE_SIZE * m_scaleFactor;
    const Size drawDimension = visibleDimension + 3,
        bufferSize = drawDimension * tileSize;

    if (bufferSize.width() > g_graphics.getMaxTextureSize() || bufferSize.height() > g_graphics.getMaxTextureSize()) {
        g_logger.traceError("reached max zoom out");
        return;
    }

    m_visibleDimension = visibleDimension;
    m_drawDimension = drawDimension;
    m_tileSize = tileSize;
    m_virtualCenterOffset = (drawDimension / 2 - Size(1)).toPoint();

    m_rectDimension = { 0, 0, bufferSize };

    g_drawPool.get<PoolFramed>(PoolType::MAP)
        ->resize(bufferSize);

    if (m_drawLights) m_lightView->resize(drawDimension, tileSize);

    m_awareRange.left = std::min<uint16_t>(g_map.getAwareRange().left, (m_drawDimension.width() / 2) - 1);
    m_awareRange.top = std::min<uint16_t>(g_map.getAwareRange().top, (m_drawDimension.height() / 2) - 1);
    m_awareRange.bottom = m_awareRange.top + 1;
    m_awareRange.right = m_awareRange.left + 1;
    m_rectCache.rect = {};

    updateViewportDirectionCache();
    updateViewport();

    refreshVisibleTiles();
    refreshVisibleCreatures();
}

void MapView::onCameraMove(const Point& /*offset*/)
{
    m_rectCache.rect = {};

    if (isFollowingCreature()) {
        updateViewport(m_followingCreature->isWalking() ? m_followingCreature->getDirection() : Otc::InvalidDirection);
    }
}

void MapView::onGlobalLightChange(const Light&)
{
    updateLight();
}

void MapView::updateLight()
{
    if (!m_drawLights) return;

    const auto cameraPosition = getCameraPosition();

    Light ambientLight = cameraPosition.z > SEA_FLOOR ? Light() : g_map.getLight();
    ambientLight.intensity = std::max<uint8_t >(m_minimumAmbientLight * 255, ambientLight.intensity);

    m_lightView->setGlobalLight(ambientLight);
}

void MapView::onFloorChange(const uint8_t /*floor*/, const uint8_t /*previousFloor*/)
{
    refreshVisibleCreatures();
    updateLight();
}

void MapView::onTileUpdate(const Position&, const ThingPtr& thing, const Otc::Operation)
{
    if (thing && thing->isCreature())
        refreshVisibleCreatures();

    refreshVisibleTiles();
}

void MapView::onFadeInFinished()
{
    refreshVisibleTiles();
}

// isVirtualMove is when the mouse is stopped, but the camera moves,
// so the onMouseMove event is triggered by sending the new tile position that the mouse is in.
void MapView::onMouseMove(const Position& mousePos, const bool /*isVirtualMove*/)
{
    { // Highlight Target System
        if (m_lastHighlightTile) {
            m_lastHighlightTile->unselect();
            m_lastHighlightTile = nullptr;
        }

        if (m_drawHighlightTarget) {
            if (m_lastHighlightTile = (m_shiftPressed ? getTopTile(mousePos) : g_map.getTile(mousePos)))
                m_lastHighlightTile->select(m_shiftPressed);
        }
    }
}

void MapView::onKeyRelease(const InputEvent& inputEvent)
{
    const bool shiftPressed = inputEvent.keyboardModifiers == Fw::KeyboardShiftModifier;
    if (shiftPressed != m_shiftPressed) {
        m_shiftPressed = shiftPressed;
        onMouseMove(m_mousePosition);
    }
}

void MapView::onMapCenterChange(const Position& /*newPos*/, const Position& /*oldPos*/)
{
    refreshVisibleTiles();
}

void MapView::lockFirstVisibleFloor(uint8_t firstVisibleFloor)
{
    m_lockedFirstVisibleFloor = firstVisibleFloor;
    refreshVisibleTiles();
}

void MapView::unlockFirstVisibleFloor()
{
    m_lockedFirstVisibleFloor = -1;
    refreshVisibleTiles();
}

void MapView::setVisibleDimension(const Size& visibleDimension)
{
    if (visibleDimension == m_visibleDimension)
        return;

    if (visibleDimension.width() % 2 != 1 || visibleDimension.height() % 2 != 1) {
        g_logger.traceError("visible dimension must be odd");
        return;
    }

    if (visibleDimension < Size(3)) {
        g_logger.traceError("reach max zoom in");
        return;
    }

    updateGeometry(visibleDimension);
}

void MapView::setFloorViewMode(FloorViewMode floorViewMode)
{
    m_floorViewMode = floorViewMode;

    resetLastCamera();
    refreshVisibleTiles();
}

void MapView::setAntiAliasingMode(const AntialiasingMode mode)
{
    g_drawPool.get<PoolFramed>(PoolType::MAP)
        ->setSmooth(mode != ANTIALIASING_DISABLED);

    m_scaleFactor = mode == ANTIALIASING_SMOOTH_RETRO ? 2.f : 1.f;

    if (m_drawLights) m_lightView->setSmooth(mode != ANTIALIASING_DISABLED);

    updateGeometry(m_visibleDimension);
}

void MapView::followCreature(const CreaturePtr& creature)
{
    m_follow = true;
    m_followingCreature = creature;
    m_lastCameraPosition = {};

    refreshVisibleTiles();
}

void MapView::setCameraPosition(const Position& pos)
{
    m_follow = false;
    m_customCameraPosition = pos;
    refreshVisibleTiles();
}

Position MapView::getPosition(const Point& point, const Size& mapSize)
{
    const Position cameraPosition = getCameraPosition();

    // if we have no camera, its impossible to get the tile
    if (!cameraPosition.isValid())
        return {};

    const Rect srcRect = calcFramebufferSource(mapSize);
    const float sh = srcRect.width() / static_cast<float>(mapSize.width());
    const float sv = srcRect.height() / static_cast<float>(mapSize.height());

    const auto framebufferPos = Point(point.x * sh, point.y * sv);
    const Point centerOffset = (framebufferPos + srcRect.topLeft()) / m_tileSize;

    const Point tilePos2D = m_virtualCenterOffset - m_drawDimension.toPoint() + centerOffset + Point(2);
    if (tilePos2D.x + cameraPosition.x < 0 && tilePos2D.y + cameraPosition.y < 0)
        return {};

    const Position position = Position(tilePos2D.x, tilePos2D.y, 0) + cameraPosition;

    if (!position.isValid())
        return {};

    return position;
}

void MapView::move(int32_t x, int32_t y)
{
    m_moveOffset.x += x;
    m_moveOffset.y += y;

    int32_t tmp = m_moveOffset.x / SPRITE_SIZE;
    bool requestTilesUpdate = false;
    if (tmp != 0) {
        m_customCameraPosition.x += tmp;
        m_moveOffset.x %= SPRITE_SIZE;
        requestTilesUpdate = true;
    }

    tmp = m_moveOffset.y / SPRITE_SIZE;
    if (tmp != 0) {
        m_customCameraPosition.y += tmp;
        m_moveOffset.y %= SPRITE_SIZE;
        requestTilesUpdate = true;
    }

    m_rectCache.rect = {};

    if (requestTilesUpdate)
        refreshVisibleTiles();

    onCameraMove(m_moveOffset);
}

Rect MapView::calcFramebufferSource(const Size& destSize)
{
    Point drawOffset = ((m_drawDimension - m_visibleDimension - Size(1)).toPoint() / 2) * m_tileSize;
    if (isFollowingCreature())
        drawOffset += m_followingCreature->getWalkOffset() * m_scaleFactor;
    else if (!m_moveOffset.isNull())
        drawOffset += m_moveOffset * m_scaleFactor;

    Size srcSize = destSize;
    const Size srcVisible = m_visibleDimension * m_tileSize;
    srcSize.scale(srcVisible, Fw::KeepAspectRatio);
    drawOffset.x += (srcVisible.width() - srcSize.width()) / 2;
    drawOffset.y += (srcVisible.height() - srcSize.height()) / 2;

    return Rect(drawOffset, srcSize);
}

uint8_t MapView::calcFirstVisibleFloor(bool checkLimitsFloorsView)
{
    uint8_t z = SEA_FLOOR;
    // return forced first visible floor
    if (m_lockedFirstVisibleFloor != -1) {
        z = m_lockedFirstVisibleFloor;
    } else {
        const Position cameraPosition = getCameraPosition();

        // this could happens if the player is not known yet
        if (cameraPosition.isValid()) {
            // if nothing is limiting the view, the first visible floor is 0
            uint8_t firstFloor = 0;

            // limits to underground floors while under sea level
            if (cameraPosition.z > SEA_FLOOR)
                firstFloor = std::max<uint8_t >(cameraPosition.z - AWARE_UNDEGROUND_FLOOR_RANGE, UNDERGROUND_FLOOR);

            // loop in 3x3 tiles around the camera
            for (int_fast32_t ix = -1; checkLimitsFloorsView && ix <= 1 && firstFloor < cameraPosition.z; ++ix) {
                for (int_fast32_t iy = -1; iy <= 1 && firstFloor < cameraPosition.z; ++iy) {
                    const Position pos = cameraPosition.translated(ix, iy);

                    // process tiles that we can look through, e.g. windows, doors
                    if ((ix == 0 && iy == 0) || ((std::abs(ix) != std::abs(iy)) && g_map.isLookPossible(pos))) {
                        Position upperPos = pos;
                        Position coveredPos = pos;

                        const auto isLookPossible = g_map.isLookPossible(pos);
                        while (coveredPos.coveredUp() && upperPos.up() && upperPos.z >= firstFloor) {
                            // check tiles physically above
                            TilePtr tile = g_map.getTile(upperPos);
                            if (tile && tile->limitsFloorsView(!isLookPossible)) {
                                firstFloor = upperPos.z + 1;
                                break;
                            }

                            // check tiles geometrically above
                            tile = g_map.getTile(coveredPos);
                            if (tile && tile->limitsFloorsView(isLookPossible)) {
                                firstFloor = coveredPos.z + 1;
                                break;
                            }
                        }
                    }
                }
            }

            z = firstFloor;
        }
    }

    // just ensure the that the floor is in the valid range
    z = std::clamp<int>(z, 0, MAX_Z);
    return z;
}

uint8_t MapView::calcLastVisibleFloor()
{
    uint8_t z = SEA_FLOOR;

    const Position cameraPosition = getCameraPosition();
    // this could happens if the player is not known yet
    if (cameraPosition.isValid()) {
        // view only underground floors when below sea level
        if (cameraPosition.z > SEA_FLOOR)
            z = cameraPosition.z + AWARE_UNDEGROUND_FLOOR_RANGE;
        else
            z = SEA_FLOOR;
    }

    if (m_lockedFirstVisibleFloor != -1)
        z = std::max<int>(m_lockedFirstVisibleFloor, z);

    // just ensure the that the floor is in the valid range
    z = std::clamp<int>(z, 0, MAX_Z);
    return z;
}

TilePtr MapView::getTopTile(Position tilePos)
{
    // we must check every floor, from top to bottom to check for a clickable tile
    TilePtr tile;

    if (m_floorViewMode == ALWAYS_WITH_TRANSPARENCY && tilePos.isInRange(m_lastCameraPosition, TRANSPARENT_FLOOR_VIEW_RANGE, TRANSPARENT_FLOOR_VIEW_RANGE))
        tile = g_map.getTile(tilePos);
    else {
        tilePos.coveredUp(tilePos.z - m_cachedFirstVisibleFloor);
        for (uint8_t i = m_cachedFirstVisibleFloor; i <= m_floorMax; ++i) {
            tile = g_map.getTile(tilePos);
            if (tile && tile->isClickable())
                break;

            tilePos.coveredDown();
            tile = nullptr;
        }
    }

    return tile;
}

Position MapView::getCameraPosition()
{
    if (isFollowingCreature())
        return m_followingCreature->getPosition();

    return m_customCameraPosition;
}

void MapView::setShader(const PainterShaderProgramPtr& shader, float fadein, float fadeout)
{
    if ((m_shader == shader))
        return;

    if (fadeout > 0.0f && m_shader) {
        m_nextShader = shader;
        m_shaderSwitchDone = false;
    } else {
        m_shader = shader;
        m_nextShader = nullptr;
        m_shaderSwitchDone = true;
    }
    m_fadeTimer.restart();
    m_fadeInTime = fadein;
    m_fadeOutTime = fadeout;

    if (shader) shader->setPosition(getCameraPosition());
}

void MapView::setDrawLights(bool enable)
{
    auto* pool = g_drawPool.get<PoolFramed>(PoolType::LIGHT);
    if (pool) pool->setEnable(enable);

    if (enable == m_drawLights) return;

    if (enable) {
        m_lightView = LightViewPtr(new LightView);
        m_lightView->resize(m_drawDimension, m_tileSize);
    }
    m_drawLights = enable;

    updateLight();
}

void MapView::updateViewportDirectionCache()
{
    for (uint8_t dir = Otc::North; dir <= Otc::InvalidDirection; ++dir) {
        AwareRange& vp = m_viewPortDirection[dir];
        vp.top = m_awareRange.top;
        vp.right = m_awareRange.right;
        vp.bottom = vp.top;
        vp.left = vp.right;

        switch (dir) {
            case Otc::North:
            case Otc::South:
                vp.top += 1;
                vp.bottom += 1;
                break;

            case Otc::West:
            case Otc::East:
                vp.right += 1;
                vp.left += 1;
                break;

            case Otc::NorthEast:
            case Otc::SouthEast:
            case Otc::NorthWest:
            case Otc::SouthWest:
                vp.left += 1;
                vp.bottom += 1;
                vp.top += 1;
                vp.right += 1;
                break;

            case Otc::InvalidDirection:
                vp.left -= 1;
                vp.right -= 1;
                break;

            default:
                break;
        }
    }
}

std::vector<CreaturePtr> MapView::getSightSpectators(const Position& centerPos, bool multiFloor)
{
    return g_map.getSpectatorsInRangeEx(centerPos, multiFloor, m_awareRange.left - 1, m_awareRange.right - 2, m_awareRange.top - 1, m_awareRange.bottom - 2);
}

std::vector<CreaturePtr> MapView::getSpectators(const Position& centerPos, bool multiFloor)
{
    return g_map.getSpectatorsInRangeEx(centerPos, multiFloor, m_awareRange.left, m_awareRange.right, m_awareRange.top, m_awareRange.bottom);
}

bool MapView::isInRange(const Position& pos, const bool ignoreZ)
{
    return getCameraPosition().isInRange(pos, m_awareRange.left - 1, m_awareRange.right - 2, m_awareRange.top - 1, m_awareRange.bottom - 2, ignoreZ);
}

bool MapView::isInRangeEx(const Position& pos, const bool ignoreZ)
{
    return getCameraPosition().isInRange(pos, m_awareRange.left, m_awareRange.right, m_awareRange.top, m_awareRange.bottom, ignoreZ);
}

void MapView::setCrosshairTexture(const std::string& texturePath)
{
    m_crosshairTexture = texturePath.empty() ? nullptr : g_textures.getTexture(texturePath);
}
