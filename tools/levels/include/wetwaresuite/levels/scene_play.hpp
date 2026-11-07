// scene play: runs a 2D scene for a quick play test, like pressing F6 in Godot, without Godot.
//
// GDScript isn't run. Instead the starter scripts the suite writes (and the ones Godot writes
// for a CharacterBody2D) are recognised: a script with is_on_floor()/gravity is a platformer
// (left/right, jump on Space/Up), one with Input.get_vector a top-down mover; SPEED and
// JUMP_VELOCITY constants are read from the script. RigidBody2D falls and lands, StaticBody2D,
// AnimatableBody2D and tile levels made in levels are solid, and Area2D reports bodies going
// in. When the method a body_entered signal is connected to calls queue_free(), the area (or
// with body.queue_free() the body, with $Name.queue_free() that node) goes away, which covers coins and pickups. Shapes collide
// as their axis-aligned boxes. The first enabled Camera2D frames the view, with its zoom and
// limits; CanvasLayer children (the HUD) stay fixed on screen.
#pragma once

#include <array>
#include <string>
#include <vector>

#include "wetwaresuite/levels/scene.hpp"
#include "wetwaresuite/raster/image.hpp"

namespace wetwaresuite::scene {

struct PlayInput {
    bool left = false, right = false, up = false, down = false, jump = false;
};

struct PlayEvent {
    double time = 0;
    std::string text;
};

struct PlayBody {
    std::string path, type;
    double x = 0, y = 0;    // world position
    double vx = 0, vy = 0;  // px per second
    bool onFloor = false, gone = false;
};

// An instanced scene's nodes and resources copied into the scene, so the result has no
// instances left (used to play a scene; also handy for flattening before export).
Scene inlineInstances(const Scene& s);

class Play {
public:
    explicit Play(const Scene& s);

    // Advances the game by `seconds` in fixed 1/60 s physics steps.
    void step(const PlayInput& in, double seconds);
    // What the game window shows now (its size is the project's window size, times `scale`).
    raster::Image frame(double scale = 1) const;

    double time() const { return time_; }
    int windowWidth() const { return winW_; }
    int windowHeight() const { return winH_; }
    const std::vector<PlayBody>& bodies() const { return bodies_; }
    const std::vector<PlayEvent>& events() const { return events_; }
    // Notes about what this play test leaves out for this scene (no script, no collision ...).
    const std::vector<std::string>& notes() const { return notes_; }
    // Areas freed so far (coins picked up).
    int freed() const { return freed_; }

private:
    struct Box {
        double x0, y0, x1, y1;
    };
    struct Solid {
        Box box;
        bool oneWay = false;
    };
    struct Mover {
        std::size_t body;         // index into bodies_
        enum Kind { Platformer, TopDown, Still, Rigid } kind = Still;
        double speed = 300, jump = -400, gravityScale = 1;
        std::vector<Box> shapes;  // relative to the body's origin
        Xform2 parentInv;         // world -> the parent's space, to write the position back
    };
    struct Area {
        std::string path;
        Box box;  // world
        std::vector<std::string> inside;
        std::vector<std::string> frees;  // node paths queue_free() takes away on entering ("@body": the body)
        bool gone = false;
        std::string method;
    };

    void steps(const PlayInput& in, double dt);
    Box worldBox(const Mover& m) const;
    void moveAxis(Mover& m, double d, bool xAxis);

    Scene scene_;  // instances inlined; body positions are written back into it to draw
    std::vector<PlayBody> bodies_;
    std::vector<Mover> movers_;
    std::vector<Solid> solids_;
    std::vector<Area> areas_;
    std::vector<PlayEvent> events_;
    std::vector<std::string> notes_;
    double time_ = 0, carry_ = 0, gravity_ = 980, killY_ = 1e5;
    int freed_ = 0;
    int winW_ = 1152, winH_ = 648;
    bool jumpHeld_ = false;
};

}  // namespace wetwaresuite::scene
