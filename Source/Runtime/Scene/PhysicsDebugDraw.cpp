#include "Runtime/Scene/PhysicsDebugDraw.h"

#include <cmath>

namespace MmdLab
{
namespace
{
constexpr int kCircleSegments = 16;
constexpr float kTwoPi = 6.28318530718f;

// Projects body-local points to the screen and records the segments between them.
class WireframeWriter
{
public:
    WireframeWriter(const DirectX::XMMATRIX& localToClip, const float width, const float height,
        const BodyMode mode, std::vector<DebugLine2D>& out)
        : localToClip_(localToClip)
        , width_(width)
        , height_(height)
        , mode_(mode)
        , out_(out)
    {
    }

    void Segment(const DirectX::XMVECTOR a, const DirectX::XMVECTOR b)
    {
        float ax;
        float ay;
        float bx;
        float by;
        if (Project(a, ax, ay) && Project(b, bx, by))
        {
            out_.push_back({ ax, ay, bx, by, mode_ });
        }
    }

    // An arc of radius `radius` about `center` in the plane spanned by unit axes `u` and `v`,
    // from angle `from` to `to` (radians).
    void Arc(const DirectX::XMVECTOR center, const DirectX::XMVECTOR u, const DirectX::XMVECTOR v,
        const float radius, const float from, const float to, const int segments)
    {
        using namespace DirectX;
        XMVECTOR previous = center + (u * std::cos(from) + v * std::sin(from)) * radius;
        for (int i = 1; i <= segments; ++i)
        {
            const float angle = from + (to - from) * static_cast<float>(i) / static_cast<float>(segments);
            const XMVECTOR next = center + (u * std::cos(angle) + v * std::sin(angle)) * radius;
            Segment(previous, next);
            previous = next;
        }
    }

private:
    bool Project(const DirectX::XMVECTOR local, float& x, float& y) const
    {
        using namespace DirectX;
        const XMVECTOR clip = XMVector4Transform(XMVectorSetW(local, 1.0f), localToClip_);
        const float w = XMVectorGetW(clip);
        if (w <= 1e-4f)
        {
            return false; // Behind (or on) the eye plane.
        }
        x = (XMVectorGetX(clip) / w * 0.5f + 0.5f) * width_;
        y = (0.5f - XMVectorGetY(clip) / w * 0.5f) * height_;
        return true;
    }

    DirectX::XMMATRIX localToClip_;
    float width_;
    float height_;
    BodyMode mode_;
    std::vector<DebugLine2D>& out_;
};
} // namespace

void AppendBodyWireframe(const PhysicsDebugBody& body, const DirectX::XMMATRIX& modelToClip,
    const float viewportWidth, const float viewportHeight, std::vector<DebugLine2D>& out)
{
    using namespace DirectX;
    const XMMATRIX localToClip = XMMatrixMultiply(XMLoadFloat4x4(&body.world), modelToClip);
    WireframeWriter writer(localToClip, viewportWidth, viewportHeight, body.mode, out);
    const XMVECTOR origin = XMVectorZero();
    const XMVECTOR x = XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f);
    const XMVECTOR y = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    const XMVECTOR z = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);

    switch (body.shape)
    {
    case BodyShape::Sphere:
    {
        const float radius = body.size[0];
        writer.Arc(origin, x, y, radius, 0.0f, kTwoPi, kCircleSegments);
        writer.Arc(origin, y, z, radius, 0.0f, kTwoPi, kCircleSegments);
        writer.Arc(origin, z, x, radius, 0.0f, kTwoPi, kCircleSegments);
        break;
    }
    case BodyShape::Box:
    {
        const XMVECTOR half = XMVectorSet(body.size[0], body.size[1], body.size[2], 0.0f);
        XMVECTOR corners[8];
        for (int i = 0; i < 8; ++i)
        {
            corners[i] = XMVectorMultiply(half, XMVectorSet(
                (i & 1) != 0 ? 1.0f : -1.0f, (i & 2) != 0 ? 1.0f : -1.0f, (i & 4) != 0 ? 1.0f : -1.0f, 0.0f));
        }
        // Each edge joins two corners that differ in exactly one axis bit.
        for (int i = 0; i < 8; ++i)
        {
            for (const int bit : { 1, 2, 4 })
            {
                if ((i & bit) == 0)
                {
                    writer.Segment(corners[i], corners[i | bit]);
                }
            }
        }
        break;
    }
    case BodyShape::Capsule:
    {
        // A cylinder of height size[1] along local +Y, capped by hemispheres of radius size[0].
        const float radius = body.size[0];
        const float halfHeight = 0.5f * body.size[1];
        const XMVECTOR top = y * halfHeight;
        const XMVECTOR bottom = y * -halfHeight;
        writer.Arc(top, z, x, radius, 0.0f, kTwoPi, kCircleSegments);
        writer.Arc(bottom, z, x, radius, 0.0f, kTwoPi, kCircleSegments);
        for (const XMVECTOR side : { x, -x, z, -z })
        {
            writer.Segment(top + side * radius, bottom + side * radius);
        }
        writer.Arc(top, x, y, radius, 0.0f, kTwoPi / 2.0f, kCircleSegments / 2);
        writer.Arc(top, z, y, radius, 0.0f, kTwoPi / 2.0f, kCircleSegments / 2);
        writer.Arc(bottom, x, -y, radius, 0.0f, kTwoPi / 2.0f, kCircleSegments / 2);
        writer.Arc(bottom, z, -y, radius, 0.0f, kTwoPi / 2.0f, kCircleSegments / 2);
        break;
    }
    }
}
} // namespace MmdLab
