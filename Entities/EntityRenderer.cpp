#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "EntityRenderer.h"

#include "../Core/Logger.h"
#include "../Core/State.h"
#include "../Core/Types.h"
#include "../Camera/Camera.h"

#include <Windows.h>
#include <gl/GL.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "../IsaacRepentance.h"

#ifndef GL_CURRENT_PROGRAM
#define GL_CURRENT_PROGRAM 0x8B8D
#endif

#ifndef GL_ACTIVE_TEXTURE
#define GL_ACTIVE_TEXTURE 0x84E0
#endif

#ifndef GL_TEXTURE0
#define GL_TEXTURE0 0x84C0
#endif

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

#ifndef GL_QUADS
#define GL_QUADS 0x0007
#endif

namespace ViewPoint
{
    namespace
    {
        constexpr float PI = 3.14159265358979323846f;
        constexpr float PIXELS_PER_TILE = 40.0f;
        constexpr float FLOOR_Y = -1.5f;
        constexpr float COLLISION_Y_OFFSET = 0.035f;
        constexpr int CAPSULE_SEGMENTS = 24;

        struct SpriteTexture
        {
            GLuint id = 0;
            int width = 0;
            int height = 0;
            const char *name = nullptr;
        };

        std::unordered_map<KAGE_Graphics_ImageBase *, SpriteTexture>
            g_SpriteTextures;

        HGLRC g_TextureContext = nullptr;

        struct WorldPoint
        {
            float x;
            float y;
            float z;
        };

        struct SpriteVertex
        {
            float x;
            float y;
            float z;
            float u;
            float v;
        };

        // ------------------------------------------------------------
        // ISAAC POSITION TO VIEWPOINT WORLD POSITION
        // ------------------------------------------------------------

        static WorldPoint IsaacToWorld(
            const Vector &position,
            Room *room)
        {
            const float roomCenterX =
                320.0f +
                (static_cast<float>(room->_gridWidth) - 15.0f) *
                    20.0f;

            const float roomCenterY =
                280.0f +
                (static_cast<float>(room->_gridHeight) - 9.0f) *
                    20.0f;

            WorldPoint result{};

            // Mirror the room-space X axis to match the room mesh and the
            // player-follow camera transform.
            result.x =
                (roomCenterX - position.x) /
                PIXELS_PER_TILE;

            result.y = FLOOR_Y;

            result.z =
                (roomCenterY - position.y) /
                PIXELS_PER_TILE;

            return result;
        }

        static float GetEntityRenderHeightOffset(
            Entity *entity)
        {
            if (!entity)
                return 0.0f;

            float offset = entity->_depthOffset;

            if (entity->_type == ENTITY_TEAR)
            {
                Entity_Tear *tear =
                    static_cast<Entity_Tear *>(entity);

                const float tearHeight =
                    tear->_height;

                const float tearRenderHeight =
                    tear->GetRenderHeight();

                if (tearHeight > 0.0f)
                    offset = tearHeight;
                else if (tearRenderHeight > 0.0f)
                    offset = tearRenderHeight;
            }

            return offset / PIXELS_PER_TILE;
        }

        // ------------------------------------------------------------
        // SPRITESHEET TEXTURE CACHE
        // ------------------------------------------------------------

        static void ClearSpriteTextureCache()
        {
            for (auto &[image, texture] : g_SpriteTextures)
            {
                if (texture.id != 0)
                {
                    glDeleteTextures(1, &texture.id);
                    texture.id = 0;
                }

                texture.width = 0;
                texture.height = 0;
                texture.name = nullptr;
            }

            g_SpriteTextures.clear();
        }

        static SpriteTexture *GetSpriteTexture(
            KAGE_Graphics_ImageBase *image)
        {
            if (!image)
                return nullptr;

            const int width =
                static_cast<int>(image->GetWidth());

            const int height =
                static_cast<int>(image->GetHeight());

            const char *imageName = image->_name;
            auto found = g_SpriteTextures.find(image);

            if (found != g_SpriteTextures.end())
            {
                if (found->second.width == width &&
                    found->second.height == height &&
                    found->second.name == imageName)
                {
                    return &found->second;
                }

                if (found->second.id != 0)
                {
                    glDeleteTextures(1, &found->second.id);
                    found->second.id = 0;
                }

                found->second.width = 0;
                found->second.height = 0;
                found->second.name = nullptr;
                g_SpriteTextures.erase(found);
            }

            if (width <= 0 || height <= 0)
                return nullptr;

            const size_t pixelCount =
                static_cast<size_t>(width) *
                static_cast<size_t>(height);

            if (pixelCount > 64u * 1024u * 1024u)
                return nullptr;

            std::vector<unsigned char> pixels(pixelCount * 4u);

            image->GetTexelRegion(
                0,
                0,
                static_cast<unsigned int>(width),
                static_cast<unsigned int>(height),
                pixels.data());

            // Do not let an old OpenGL error invalidate this upload.
            while (glGetError() != GL_NO_ERROR)
            {
            }

            SpriteTexture texture{};
            texture.width = width;
            texture.height = height;
            texture.name = image->_name;

            glGenTextures(1, &texture.id);

            if (!texture.id)
                return nullptr;

            glBindTexture(GL_TEXTURE_2D, texture.id);

            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

            glTexParameteri(
                GL_TEXTURE_2D,
                GL_TEXTURE_MIN_FILTER,
                GL_NEAREST);

            glTexParameteri(
                GL_TEXTURE_2D,
                GL_TEXTURE_MAG_FILTER,
                GL_NEAREST);

            glTexParameteri(
                GL_TEXTURE_2D,
                GL_TEXTURE_WRAP_S,
                GL_CLAMP_TO_EDGE);

            glTexParameteri(
                GL_TEXTURE_2D,
                GL_TEXTURE_WRAP_T,
                GL_CLAMP_TO_EDGE);

            glTexImage2D(
                GL_TEXTURE_2D,
                0,
                GL_RGBA,
                width,
                height,
                0,
                GL_RGBA,
                GL_UNSIGNED_BYTE,
                pixels.data());

            if (glGetError() != GL_NO_ERROR)
            {
                glDeleteTextures(1, &texture.id);
                return nullptr;
            }

            auto inserted =
                g_SpriteTextures.emplace(image, texture);

            return &inserted.first->second;
        }

        // ------------------------------------------------------------
        // SPRITE COLOR
        // ------------------------------------------------------------

        static void GetSpriteColor(
            const ANM2 *sprite,
            const LayerState *layer,
            const AnimationFrame *frame,
            float &red,
            float &green,
            float &blue,
            float &alpha)
        {
            red = 1.0f;
            green = 1.0f;
            blue = 1.0f;
            alpha = 1.0f;

            if (sprite)
            {
                red *= sprite->_color._tint[0];
                green *= sprite->_color._tint[1];
                blue *= sprite->_color._tint[2];
                alpha *= sprite->_color._tint[3];
            }

            if (layer)
            {
                red *= layer->_color._tint[0];
                green *= layer->_color._tint[1];
                blue *= layer->_color._tint[2];
                alpha *= layer->_color._tint[3];
            }

            if (frame)
            {
                red *= frame->color._tint[0];
                green *= frame->color._tint[1];
                blue *= frame->color._tint[2];
                alpha *= frame->color._tint[3];
            }

            red = std::max(0.0f, std::min(1.0f, red));
            green = std::max(0.0f, std::min(1.0f, green));
            blue = std::max(0.0f, std::min(1.0f, blue));
            alpha = std::max(0.0f, std::min(1.0f, alpha));
        }

        // ------------------------------------------------------------
        // SPRITE QUAD GENERATION
        // ------------------------------------------------------------

        static void RotateSpritePoint(
            float &x,
            float &y,
            float radians)
        {
            if (std::fabs(radians) < 0.0001f)
                return;

            const float cosine = std::cos(radians);
            const float sine = std::sin(radians);

            const float oldX = x;
            const float oldY = y;

            x = oldX * cosine - oldY * sine;
            y = oldX * sine + oldY * cosine;
        }

        static bool BuildSpriteQuad(
            Entity *entity,
            Room *room,
            ANM2 *sprite,
            LayerState *layer,
            AnimationFrame *frame,
            SpriteTexture *texture,
            float cameraRightX,
            float cameraRightZ,
            SpriteVertex (&vertices)[4])
        {
            if (!entity || !room || !sprite ||
                !layer || !frame || !texture)
            {
                return false;
            }

            if (frame->width <= 0.0f ||
                frame->height <= 0.0f)
            {
                return false;
            }

            const Vector *entityPosition =
                entity->GetPosition();

            if (!entityPosition)
                return false;

            const WorldPoint base =
                IsaacToWorld(*entityPosition, room);

            const float entityHeightOffset =
                GetEntityRenderHeightOffset(entity);

            // Atlas rectangle.
            const float cropX = frame->crop.x;
            const float cropY = frame->crop.y;
            const float cropWidth = frame->width;
            const float cropHeight = frame->height;

            if (cropX < 0.0f ||
                cropY < 0.0f ||
                cropX + cropWidth > texture->width ||
                cropY + cropHeight > texture->height)
            {
                return false;
            }

            // GetTexelRegion returns image pixels in top-to-bottom order.
            // OpenGL places the first uploaded row at the bottom of the texture.
            float u0 = cropX / static_cast<float>(texture->width);
            float u1 = (cropX + cropWidth) /
                       static_cast<float>(texture->width);

            float v0 = cropY / static_cast<float>(texture->height);
            float v1 = (cropY + cropHeight) /
                       static_cast<float>(texture->height);

            if (layer->_flipX != sprite->_flipX)
                std::swap(u0, u1);

            if (layer->_flipY != sprite->_flipY)
                std::swap(v0, v1);

            // Combine the frame, ANM2 and layer scales.
            const float scaleX =
                frame->scale.x * sprite->_scale.x;

            const float scaleY =
                frame->scale.y * sprite->_scale.y;

            const float width =
                cropWidth * std::fabs(scaleX);

            const float height =
                cropHeight * std::fabs(scaleY);

            if (width <= 0.001f || height <= 0.001f)
                return false;

            // The frame pivot is the local origin.
            // Positive local Y points down in Isaac's 2D space.
            const float left =
                frame->pos.x +
                layer->_pos.x +
                sprite->_offset.x -
                frame->pivot.x;

            const float top =
                frame->pos.y +
                layer->_pos.y +
                sprite->_offset.y -
                frame->pivot.y;

            // Apply the sprite's local rotation.
            const float rotation =
                (frame->rotation +
                 layer->_rotation +
                 sprite->_rotation) *
                PI / 180.0f;

            float localX[4] =
                {
                    left,
                    left + width,
                    left + width,
                    left};

            float localY[4] =
                {
                    top,
                    top,
                    top + height,
                    top + height};

            for (int i = 0; i < 4; ++i)
            {
                RotateSpritePoint(
                    localX[i],
                    localY[i],
                    rotation);
            }

            const float u[4] = {u0, u1, u1, u0};
            const float v[4] = {v0, v0, v1, v1};

            for (int i = 0; i < 4; ++i)
            {
                const float horizontal =
                    localX[i] / PIXELS_PER_TILE;

                const float vertical =
                    localY[i] / PIXELS_PER_TILE;

                vertices[i].x =
                    base.x +
                    cameraRightX * horizontal;

                vertices[i].y =
                    base.y + entityHeightOffset - vertical;

                vertices[i].z =
                    base.z +
                    cameraRightZ * horizontal;

                vertices[i].u = u[i];
                vertices[i].v = v[i];
            }

            // A negative scale mirrors the geometry as well as
            // the atlas coordinates.
            if (scaleX < 0.0f)
            {
                std::swap(vertices[0], vertices[1]);
                std::swap(vertices[3], vertices[2]);
            }

            if (scaleY < 0.0f)
            {
                std::swap(vertices[0], vertices[3]);
                std::swap(vertices[1], vertices[2]);
            }

            return true;
        }

        static int GetDoorSlotForDoor(
            const Room *room,
            const GridEntity_Door *door)
        {
            if (!room || !door)
                return -1;

            for (int slot = 0; slot < 8; ++slot)
            {
                if (room->_doors[slot] == door)
                    return slot;
            }

            return -1;
        }

        enum DoorWallSide
        {
            DOOR_WALL_NORTH = 0,
            DOOR_WALL_EAST = 1,
            DOOR_WALL_SOUTH = 2,
            DOOR_WALL_WEST = 3
        };

        static DoorWallSide GetDoorWallSide(int doorSlot)
        {
            switch (doorSlot)
            {
                case DOOR_SLOT_UP0:
                case DOOR_SLOT_UP1:
                    return DOOR_WALL_NORTH;

                case DOOR_SLOT_RIGHT0:
                case DOOR_SLOT_RIGHT1:
                    return DOOR_WALL_EAST;

                case DOOR_SLOT_DOWN0:
                case DOOR_SLOT_DOWN1:
                    return DOOR_WALL_SOUTH;

                case DOOR_SLOT_LEFT0:
                case DOOR_SLOT_LEFT1:
                default:
                    return DOOR_WALL_WEST;
            }
        }

        static float GetDoorRenderRotationDegrees(int doorSlot)
        {
            switch (GetDoorWallSide(doorSlot))
            {
                case DOOR_WALL_NORTH:
                    return 0.0f;
                case DOOR_WALL_EAST:
                    return 270.0f;
                case DOOR_WALL_SOUTH:
                    return 180.0f;
                case DOOR_WALL_WEST:
                    return 90.0f;
                default:
                    return 0.0f;
            }
        }

        static WorldPoint GetDoorRenderWorldPosition(
            Room *room,
            GridEntity_Door *door,
            int doorSlot)
        {
            WorldPoint result{};

            if (!room || !door)
                return result;

            Vector position(0.0f, 0.0f);
            door->GetRenderPosition(&position);
            result = IsaacToWorld(position, room);

            switch (GetDoorWallSide(doorSlot))
            {
                case DOOR_WALL_NORTH:
                    result.z -= 2.59f;
                    result.x += 0.50f;
                    result.y = FLOOR_Y + 0.0f;
                    break;
                case DOOR_WALL_EAST:
                    result.x += -2.05f;
                    result.z -= 4.5f;
                    result.y = FLOOR_Y + 0.0f;
                    break;
                case DOOR_WALL_SOUTH:
                    result.x += 0.5f;
                    result.z += -4.65f;
                    result.y = FLOOR_Y + 0.02f;
                    break;
                case DOOR_WALL_WEST:
                    result.x -= -2.05f;
                    result.z -= 4.5f;
                    result.y = FLOOR_Y + -0.5f;
                    break;
                default:
                    result.y = FLOOR_Y + 0.24f;
                    break;
            }

            return result;
        }

        static bool BuildDoorSpriteQuad(
            Room *room,
            ANM2 *sprite,
            LayerState *layer,
            AnimationFrame *frame,
            SpriteTexture *texture,
            const WorldPoint &base,
            int doorSlot,
            float extraRotationDegrees,
            float cameraRightX,
            float cameraRightZ,
            SpriteVertex (&vertices)[4])
        {
            if (!room || !sprite || !layer || !frame || !texture)
                return false;

            if (frame->width <= 0.0f ||
                frame->height <= 0.0f)
            {
                return false;
            }

            const float cropX = frame->crop.x;
            const float cropY = frame->crop.y;
            const float cropWidth = frame->width;
            const float cropHeight = frame->height;

            if (cropX < 0.0f ||
                cropY < 0.0f ||
                cropX + cropWidth > texture->width ||
                cropY + cropHeight > texture->height)
            {
                return false;
            }

            float u0 = cropX / static_cast<float>(texture->width);
            float u1 = (cropX + cropWidth) /
                       static_cast<float>(texture->width);

            float v0 = cropY / static_cast<float>(texture->height);
            float v1 = (cropY + cropHeight) /
                       static_cast<float>(texture->height);

            if (layer->_flipX != sprite->_flipX)
                std::swap(u0, u1);

            if (layer->_flipY != sprite->_flipY)
                std::swap(v0, v1);

            const float scaleX =
                frame->scale.x * sprite->_scale.x;

            const float scaleY =
                frame->scale.y * sprite->_scale.y;

            const float width =
                cropWidth * std::fabs(scaleX) * 2.0f;

            const float height =
                cropHeight * std::fabs(scaleY) * 2.5f;

            if (width <= 0.001f || height <= 0.001f)
                return false;

            const float left =
                frame->pos.x +
                layer->_pos.x +
                sprite->_offset.x -
                frame->pivot.x;

            const float top =
                frame->pos.y +
                layer->_pos.y +
                sprite->_offset.y -
                frame->pivot.y;

            const float rotation =
                (frame->rotation +
                 layer->_rotation +
                 sprite->_rotation +
                 extraRotationDegrees) *
                PI / 180.0f;

            float localX[4] =
                {
                    left,
                    left + width,
                    left + width,
                    left};

            float localY[4] =
                {
                    top,
                    top,
                    top + height,
                    top + height};

            for (int i = 0; i < 4; ++i)
            {
                RotateSpritePoint(
                    localX[i],
                    localY[i],
                    rotation);
            }

            const float u[4] = {u0, u1, u1, u0};
            const float v[4] = {v0, v0, v1, v1};
            const float wallDepth = 0.14f;
            const DoorWallSide wallSide = GetDoorWallSide(doorSlot);

            for (int i = 0; i < 4; ++i)
            {
                const float horizontal =
                    localX[i] / PIXELS_PER_TILE;

                const float vertical =
                    localY[i] / PIXELS_PER_TILE;

                switch (wallSide)
                {
                    case DOOR_WALL_NORTH:
                        vertices[i].x = base.x + horizontal;
                        vertices[i].z = base.z - wallDepth;
                        break;
                    case DOOR_WALL_EAST:
                        vertices[i].x = base.x + wallDepth;
                        vertices[i].z = base.z - horizontal;
                        break;
                    case DOOR_WALL_SOUTH:
                        vertices[i].x = base.x - horizontal;
                        vertices[i].z = base.z + wallDepth;
                        break;
                    case DOOR_WALL_WEST:
                    default:
                        vertices[i].x = base.x - wallDepth;
                        vertices[i].z = base.z + horizontal;
                        break;
                }

                vertices[i].y = base.y - vertical;
                vertices[i].u = u[i];
                vertices[i].v = v[i];
            }

            if (scaleX < 0.0f)
            {
                std::swap(vertices[0], vertices[1]);
                std::swap(vertices[3], vertices[2]);
            }

            if (scaleY < 0.0f)
            {
                std::swap(vertices[0], vertices[3]);
                std::swap(vertices[1], vertices[2]);
            }

            return true;
        }

        static void RenderDoorSprite(
            Room *room,
            GridEntity_Door *door,
            float cameraRightX,
            float cameraRightZ)
        {
            if (!room || !door)
                return;

            const int doorSlot = GetDoorSlotForDoor(room, door);
            const float extraRotationDegrees = GetDoorRenderRotationDegrees(doorSlot);
            const WorldPoint base =
                GetDoorRenderWorldPosition(room, door, doorSlot);

            ANM2 *sprite = &door->_extraSprite;
            if (!sprite->_loaded ||
                !sprite->_layerState ||
                sprite->_layerCount == 0)
            {
                sprite = &door->_sprite;
            }

            if (!sprite->_loaded ||
                !sprite->_layerState ||
                sprite->_layerCount == 0)
            {
                return;
            }

            AnimationState *animationState = sprite->GetAnimationState();
            if (!animationState || !animationState->_animData)
                return;

            AnimationData *animationData = animationState->_animData;
            if (!animationData->_animLayers || animationData->_layerCount <= 0)
                return;

            for (int i = 0; i < animationData->_layerCount; ++i)
            {
                AnimationLayer *animationLayer =
                    animationData->GetLayerByOrder(i);

                if (!animationLayer)
                    continue;

                if (!animationState->_layerFrames ||
                    !animationState->_animData)
                {
                    continue;
                }

                const int layerID = animationLayer->_layerID;
                if (layerID < 0 || layerID >= 64)
                    continue;

                const int frameIndex = animationState->_layerFrames[layerID];
                if (frameIndex < 0)
                    continue;

                AnimationFrame *frame = animationLayer->GetFrame(frameIndex);
                if (!frame)
                    continue;

                if (!animationLayer->IsVisible() || !frame->visible)
                    continue;

                LayerState *layer = sprite->GetLayer(layerID);
                if (!layer || !layer->_visible)
                    continue;

                if (!layer->_spriteSheetLoaded)
                    continue;

                KAGE_SmartPointer_ImageBase sheet =
                    layer->GetSpriteSheet();

                if (!sheet.image)
                    continue;

                SpriteTexture *texture = GetSpriteTexture(sheet.image);
                if (!texture || !texture->id)
                    continue;

                SpriteVertex vertices[4]{};
                if (!BuildDoorSpriteQuad(
                        room,
                        sprite,
                        layer,
                        frame,
                        texture,
                        base,
                        doorSlot,
                        extraRotationDegrees,
                        cameraRightX,
                        cameraRightZ,
                        vertices))
                {
                    continue;
                }

                float red;
                float green;
                float blue;
                float alpha;

                GetSpriteColor(
                    sprite,
                    layer,
                    frame,
                    red,
                    green,
                    blue,
                    alpha);

                if (alpha <= 0.001f)
                    continue;

                glBindTexture(GL_TEXTURE_2D, texture->id);
                glColor4f(red, green, blue, alpha);

                glBegin(GL_QUADS);
                for (int v = 0; v < 4; ++v)
                {
                    glTexCoord2f(vertices[v].u, vertices[v].v);
                    glVertex3f(vertices[v].x, vertices[v].y, vertices[v].z);
                }
                glEnd();
            }
        }

        // ------------------------------------------------------------
        // ONE ANIMATION LAYER
        // ------------------------------------------------------------


static void RenderSpriteLayer(
    Entity* entity,
    Room* room,
    ANM2* sprite,
    AnimationState* animationState,
    AnimationLayer* animationLayer,
    float cameraRightX,
    float cameraRightZ)
{
    if (!entity || !room || !sprite ||
        !animationState || !animationLayer)
        return;

    if (!animationState->_layerFrames ||
        !animationState->_animData)
        return;

    const int layerID = animationLayer->_layerID;

    if (layerID < 0 || layerID >= 64)
        return;

    // Animation frame storage is indexed by layer ID,
    // not by animation draw order.
    const int frameIndex =
        animationState->_layerFrames[layerID];

    if (frameIndex < 0)
        return;

    AnimationFrame* frame =
        animationLayer->GetFrame(frameIndex);

    if (!frame)
        return;

    if (!animationLayer->IsVisible() ||
        !frame->visible)
        return;

    // Get the corresponding ANM2 layer state.
    LayerState* layer =
        sprite->GetLayer(layerID);

    if (!layer || !layer->_visible)
        return;

    if (!layer->_spriteSheetLoaded)
        return;

    KAGE_SmartPointer_ImageBase sheet =
        layer->GetSpriteSheet();

    if (!sheet.image)
        return;

    SpriteTexture* texture =
        GetSpriteTexture(sheet.image);

    if (!texture || !texture->id)
        return;

    SpriteVertex vertices[4]{};

    if (!BuildSpriteQuad(
            entity,
            room,
            sprite,
            layer,
            frame,
            texture,
            cameraRightX,
            cameraRightZ,
            vertices))
        return;

    float red;
    float green;
    float blue;
    float alpha;

    GetSpriteColor(
        sprite,
        layer,
        frame,
        red,
        green,
        blue,
        alpha);

    if (alpha <= 0.001f)
        return;

    glBindTexture(GL_TEXTURE_2D, texture->id);
    glColor4f(red, green, blue, alpha);

    glBegin(GL_QUADS);

    for (int i = 0; i < 4; ++i)
    {
        glTexCoord2f(vertices[i].u, vertices[i].v);
        glVertex3f(vertices[i].x, vertices[i].y, vertices[i].z);
    }

    glEnd();
}

        // ------------------------------------------------------------
        // ENTITY SPRITE
        // ------------------------------------------------------------

        static void RenderEntitySprite(
            Entity *entity,
            Room *room,
            float cameraRightX,
            float cameraRightZ)
        {
            if (!entity || !room)
                return;

            if (!entity->_exists ||
                entity->_dead ||
                !entity->_visible)
            {
                return;
            }

            ANM2 *sprite = &entity->_sprite;

            if (!sprite->_loaded ||
                !sprite->_layerState ||
                sprite->_layerCount == 0)
            {
                return;
            }

            AnimationState *animationState =
                sprite->GetAnimationState();

            if (!animationState ||
                !animationState->_animData)
            {
                return;
            }

            AnimationData *animationData =
                animationState->_animData;

            if (!animationData->_animLayers ||
                animationData->_layerCount <= 0)
            {
                return;
            }

            // AnimationData's order is the draw order.
            // The ANM2 layer state is retrieved separately by ID.
            for (int i = 0; i < animationData->_layerCount; ++i)
            {
                AnimationLayer* animationLayer =
                    animationData->GetLayerByOrder(i);

                if (!animationLayer)
                    continue;

                RenderSpriteLayer(
                    entity,
                    room,
                    sprite,
                    animationState,
                    animationLayer,
                    cameraRightX,
                    cameraRightZ);
            }
        }

        // ------------------------------------------------------------
        // COLLISION CAPSULE
        // ------------------------------------------------------------

        static void RenderCapsuleCircle(
            float centerX,
            float centerZ,
            float radius)
        {
            glBegin(GL_LINE_LOOP);

            for (int i = 0; i < CAPSULE_SEGMENTS; ++i)
            {
                const float angle =
                    2.0f * PI *
                    static_cast<float>(i) /
                    static_cast<float>(CAPSULE_SEGMENTS);

                glVertex3f(
                    centerX + std::cos(angle) * radius,
                    FLOOR_Y + COLLISION_Y_OFFSET,
                    centerZ + std::sin(angle) * radius);
            }

            glEnd();
        }

        static void RenderCollisionCapsule(
            Entity *entity,
            Room *room)
        {
            if (!entity || !room)
                return;

            if (!entity->_exists || entity->_dead)
                return;

            Vector offset(0.0f, 0.0f);

            const Capsule capsule =
                entity->GetCollisionCapsule(&offset);

            const WorldPoint start =
                IsaacToWorld(capsule._vec2, room);

            const WorldPoint end =
                IsaacToWorld(capsule._vec3, room);

            const float radius =
                std::fabs(capsule._unkFloat1) /
                PIXELS_PER_TILE;

            if (radius <= 0.001f)
                return;

            const float dx = end.x - start.x;
            const float dz = end.z - start.z;

            const float length =
                std::sqrt(dx * dx + dz * dz);

            glColor4f(
                0.1f,
                1.0f,
                0.2f,
                1.0f);

            glLineWidth(1.5f);

            if (length <= 0.001f)
            {
                RenderCapsuleCircle(
                    start.x,
                    start.z,
                    radius);

                return;
            }

            const float normalX = -dz / length;
            const float normalZ = dx / length;

            const float offsetX = normalX * radius;
            const float offsetZ = normalZ * radius;

            const float y =
                FLOOR_Y + COLLISION_Y_OFFSET;

            glBegin(GL_LINES);

            glVertex3f(
                start.x + offsetX,
                y,
                start.z + offsetZ);

            glVertex3f(
                end.x + offsetX,
                y,
                end.z + offsetZ);

            glVertex3f(
                start.x - offsetX,
                y,
                start.z - offsetZ);

            glVertex3f(
                end.x - offsetX,
                y,
                end.z - offsetZ);

            glEnd();

            RenderCapsuleCircle(
                start.x,
                start.z,
                radius);

            RenderCapsuleCircle(
                end.x,
                end.z,
                radius);
        }

        // ------------------------------------------------------------
        // RENDER STATE
        // ------------------------------------------------------------

        struct RenderState
        {
            GLint program = 0;
            GLint matrixMode = GL_MODELVIEW;
            GLint activeTexture = GL_TEXTURE0;
            bool matricesPushed = false;
            bool attributesPushed = false;
        };

        static bool BeginEntityRendering(RenderState &state)
        {
            GLint viewport[4] = {};

            glGetIntegerv(GL_VIEWPORT, viewport);

            if (viewport[2] <= 0 || viewport[3] <= 0)
                return false;

            GLint depthBits = 0;

            glGetIntegerv(GL_DEPTH_BITS, &depthBits);

            if (depthBits <= 0)
                return false;

            glGetIntegerv(
                GL_CURRENT_PROGRAM,
                &state.program);

            glGetIntegerv(
                GL_MATRIX_MODE,
                &state.matrixMode);

            glGetIntegerv(
                GL_ACTIVE_TEXTURE,
                &state.activeTexture);

            glPushAttrib(GL_ALL_ATTRIB_BITS);
            state.attributesPushed = true;

            glMatrixMode(GL_PROJECTION);
            glPushMatrix();

            glMatrixMode(GL_MODELVIEW);
            glPushMatrix();

            state.matricesPushed = true;

            // The entity renderer uses the compatibility pipeline.
            if (g_UseProgram)
                g_UseProgram(0);

            if (g_ActiveTexture)
                g_ActiveTexture(GL_TEXTURE0);

            glDisable(GL_CULL_FACE);
            glDisable(GL_LIGHTING);
            glDisable(GL_FOG);
            glDisable(GL_SCISSOR_TEST);
            glDisable(GL_STENCIL_TEST);

            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_LEQUAL);
            glDepthMask(GL_TRUE);

            glEnable(GL_BLEND);
            glDepthMask(GL_FALSE);

            glBlendFunc(
                GL_SRC_ALPHA,
                GL_ONE_MINUS_SRC_ALPHA);

            glDisable(GL_ALPHA_TEST);

            glEnable(GL_TEXTURE_2D);

            const double aspect =
                static_cast<double>(viewport[2]) /
                static_cast<double>(viewport[3]);

            const double nearPlane =
                g_Camera.nearPlane;

            const double farPlane =
                g_Camera.farPlane;

            const double halfFov =
                static_cast<double>(g_Camera.fieldOfView) *
                PI / 360.0;

            const double halfHeight =
                nearPlane * std::tan(halfFov);

            const double halfWidth =
                halfHeight * aspect;

            glMatrixMode(GL_PROJECTION);
            glLoadIdentity();

            glFrustum(
                -halfWidth,
                halfWidth,
                -halfHeight,
                halfHeight,
                nearPlane,
                farPlane);

            glMatrixMode(GL_MODELVIEW);
            glLoadIdentity();

            glRotatef(
                -g_Camera.pitch,
                1.0f,
                0.0f,
                0.0f);

            glRotatef(
                -g_Camera.yaw,
                0.0f,
                1.0f,
                0.0f);

            glTranslatef(
                -g_Camera.position.x,
                -g_Camera.position.y,
                -g_Camera.position.z);

            return true;
        }

        static void EndEntityRendering(const RenderState &state)
        {
            if (state.matricesPushed)
            {
                glMatrixMode(GL_MODELVIEW);
                glPopMatrix();

                glMatrixMode(GL_PROJECTION);
                glPopMatrix();
            }

            if (state.attributesPushed)
                glPopAttrib();

            if (g_UseProgram)
            {
                g_UseProgram(
                    static_cast<GLuint>(state.program));
            }

            if (g_ActiveTexture)
            {
                g_ActiveTexture(
                    static_cast<GLenum>(state.activeTexture));
            }

            glMatrixMode(
                static_cast<GLenum>(state.matrixMode));
        }
    }

    // ================================================================
    // PUBLIC ENTRY POINT
    // ================================================================

    void RenderEntities()
    {
        if (!g_Camera.enabled || !g_Game)
            return;

        Room *room = g_Game->GetCurrentRoom();

        if (!room)
            return;

        EntityList *entityList =
            room->GetEntityList();

        if (!entityList)
            return;

        EntityList_EL *renderList =
            entityList->GetRenderEL();

        if (!renderList ||
            !renderList->_data ||
            renderList->_size == 0)
        {
            return;
        }

        const HGLRC currentContext =
            wglGetCurrentContext();

        if (!currentContext)
            return;

        if (currentContext != g_TextureContext)
        {
            // Texture IDs from a previous context cannot be reused.
            ClearSpriteTextureCache();
            g_TextureContext = currentContext;
        }
        else if (g_SpriteTextures.size() > 1024)
        {
            // Sprite-sheet pointers can be recycled by the game engine, but we do
            // not need to rebuild the entire atlas cache every frame. A bounded
            // invalidation keeps stale textures from lingering without stalling the
            // render loop on a full OpenGL reupload for every sprite sheet.
            ClearSpriteTextureCache();
        }

        RenderState state{};

        if (!BeginEntityRendering(state))
            return;

        const float yawRadians =
            g_Camera.yaw * PI / 180.0f;

        const float cameraRightX =
            std::cos(yawRadians);

        const float cameraRightZ =
            -std::sin(yawRadians);

        // ------------------------------------------------------------
        // DOOR SPRITES
        // ------------------------------------------------------------

        glEnable(GL_TEXTURE_2D);
        glEnable(GL_BLEND);

        for (int slot = 0; slot < 8; ++slot)
        {
            GridEntity_Door *door =
                static_cast<GridEntity_Door *>(room->_doors[slot]);

            if (!door)
                continue;

            RenderDoorSprite(
                room,
                door,
                cameraRightX,
                cameraRightZ);
        }

        // ------------------------------------------------------------
        // ENTITY SPRITES
        // ------------------------------------------------------------

        glEnable(GL_TEXTURE_2D);
        glEnable(GL_BLEND);

        std::vector<Entity *> orderedEntities;
        orderedEntities.reserve(renderList->_size);

        for (unsigned int i = 0;
             i < renderList->_size;
             ++i)
        {
            Entity *entity = renderList->_data[i];

            if (!entity ||
                !entity->_exists ||
                entity->_dead ||
                !entity->_visible)
            {
                continue;
            }

            const Vector *entityPosition = entity->GetPosition();

            if (!entityPosition)
                continue;

            orderedEntities.push_back(entity);
        }

        std::sort(
            orderedEntities.begin(),
            orderedEntities.end(),
            [room](Entity *left, Entity *right)
            {
                const Vector *leftPos = left ? left->GetPosition() : nullptr;
                const Vector *rightPos = right ? right->GetPosition() : nullptr;

                if (!leftPos && !rightPos)
                    return false;
                if (!leftPos)
                    return false;
                if (!rightPos)
                    return true;

                const WorldPoint leftWorld = IsaacToWorld(*leftPos, room);
                const WorldPoint rightWorld = IsaacToWorld(*rightPos, room);

                const float leftDx = leftWorld.x - g_Camera.position.x;
                const float leftDz = leftWorld.z - g_Camera.position.z;
                const float leftDy = (leftWorld.y + GetEntityRenderHeightOffset(left)) - g_Camera.position.y;

                const float rightDx = rightWorld.x - g_Camera.position.x;
                const float rightDz = rightWorld.z - g_Camera.position.z;
                const float rightDy = (rightWorld.y + GetEntityRenderHeightOffset(right)) - g_Camera.position.y;

                const float leftDistance =
                    leftDx * leftDx + leftDy * leftDy + leftDz * leftDz;
                const float rightDistance =
                    rightDx * rightDx + rightDy * rightDy + rightDz * rightDz;

                return leftDistance > rightDistance;
            });

        for (Entity *entity : orderedEntities)
        {
            RenderEntitySprite(
                entity,
                room,
                cameraRightX,
                cameraRightZ);
        }

        glDepthMask(GL_TRUE);

        // ------------------------------------------------------------
        // COLLISION OUTLINES
        // ------------------------------------------------------------

        glDisable(GL_TEXTURE_2D);
        glDisable(GL_BLEND);

        if (g_UseProgram)
            g_UseProgram(0);

        for (unsigned int i = 0;
             i < renderList->_size;
             ++i)
        {
            Entity *entity = renderList->_data[i];

            if (!entity)
                continue;

            RenderCollisionCapsule(
                entity,
                room);
        }

        EndEntityRendering(state);
    }
}