#include <stdint.h>
#include <iostream>
#include <map>
#include <vector>

#include "Core.h"
#include "Console.h"
#include "PluginManager.h"

#include "modules/EventManager.h"
#include "modules/Maps.h"
#include "modules/World.h"
#include "modules/MapCache.h"
#include "modules/Gui.h"
#include "modules/Units.h"
#include "modules/Screen.h"

#include "df/block_square_event_frozen_liquidst.h"
#include "df/construction.h"
#include "df/deep_vein_hollow.h"
#include "df/divine_treasure.h"
#include "df/encased_horror.h"
#include "df/world.h"

#include <unordered_set>

using MapExtras::MapCache;

using std::string;
using std::vector;

using namespace DFHack;
using namespace df::enums;

DFHACK_PLUGIN("reveal");
DFHACK_PLUGIN_IS_ENABLED(is_active);

REQUIRE_GLOBAL(world);

/*
 * Anything that might reveal Hell or trigger gemstone pillar events is unsafe.
 */
bool isSafe(df::coord c, const std::unordered_set<df::coord> & trigger_cache)
{
    // convert to block coordinates
    c.x >>= 4;
    c.y >>= 4;

    // Don't reveal blocks that contain trigger events
    if (trigger_cache.contains(c))
        return false;

    t_feature local_feature;
    t_feature global_feature;
    // get features of block
    // error -> obviously not safe to manipulate
    if(!Maps::ReadFeatures(c.x,c.y,c.z,&local_feature,&global_feature))
        return false;

    // Adamantine tubes and temples lead to Hell
    if (local_feature.type == feature_type::deep_special_tube || local_feature.type == feature_type::deep_surface_portal)
        return false;
    // And Hell *is* Hell.
    if (global_feature.type == feature_type::underworld_from_layer)
        return false;
    // otherwise it's safe.
    return true;
}

struct hideblock
{
    df::coord c;
    uint8_t hiddens [16][16];
};

// the saved data. we keep map size to check if things still match
uint32_t x_max, y_max, z_max;
vector <hideblock> hidesaved;
bool nopause_state = false;

enum revealstate
{
    NOT_REVEALED,
    REVEALED,
    SAFE_REVEALED,
    DEMON_REVEALED
};

revealstate revealed = NOT_REVEALED;

command_result reveal(color_ostream &out, vector<string> & params);
command_result unreveal(color_ostream &out, vector<string> & params);
command_result revtoggle(color_ostream &out, vector<string> & params);
command_result revflood(color_ostream &out, vector<string> & params);
command_result revforget(color_ostream &out, vector<string> & params);
command_result nopause(color_ostream &out, vector<string> & params);

DFhackCExport command_result plugin_init ( color_ostream &out, vector <PluginCommand> &commands)
{
    commands.push_back(PluginCommand(
        "reveal",
        "Reveal the map.",
        reveal));
    commands.push_back(PluginCommand(
        "unreveal",
        "Revert a revealed map to its unrevealed state.",
        unreveal));
    commands.push_back(PluginCommand(
        "revtoggle",
        "Switch betwen reveal and unreveal.",
        revtoggle));
    commands.push_back(PluginCommand(
        "revflood",
        "Hide all, then reveal tiles reachable from the cursor.",
        revflood));
    commands.push_back(PluginCommand(
        "revforget",
        "Forget the current reveal data.",
        revforget));
    commands.push_back(PluginCommand(
        "nopause",
        "Disable manual and automatic pausing.",
        nopause));
    return CR_OK;
}

DFhackCExport command_result plugin_onupdate ( color_ostream &out )
{
    t_gamemodes gm;
    World::ReadGameMode(gm);
    if(gm.g_mode == game_mode::DWARF)
    {
        // if the map is revealed and we're in fortress mode, force the game to pause.
        if(revealed == REVEALED)
        {
            World::SetPauseState(true);
        }
        else if(nopause_state)
        {
            World::SetPauseState(false);
        }
    }
    return CR_OK;
}

DFhackCExport command_result plugin_shutdown ( color_ostream &out )
{
    return CR_OK;
}

command_result nopause (color_ostream &out, vector <string> & parameters)
{
    if (parameters.size() == 1 && (parameters[0] == "0" || parameters[0] == "1"))
    {
        if (parameters[0] == "0")
            nopause_state = 0;
        else
            nopause_state = 1;
        is_active = nopause_state || (revealed == REVEALED);
        out.print("nopause %sactivated.\n", (nopause_state ? "" : "de"));
    }
    else
    {
        out.print("Disable pausing (doesn't affect pause forced by reveal).\nActivate with 'nopause 1', deactivate with 'nopause 0'.\nCurrent state: %d.\n", nopause_state);
    }

    return CR_OK;
}

void revealAdventure(color_ostream &out, const std::unordered_set<df::coord> & trigger_cache)
{
    for (size_t i = 0; i < world->map.map_blocks.size(); i++)
    {
        df::map_block *block = world->map.map_blocks[i];
        // in 'no-hell'/'safe' mode, don't reveal blocks with hell and adamantine
        if (!isSafe(block->map_pos, trigger_cache))
            continue;
        designations40d & designations = block->designation;
        // for each tile in block
        for (uint32_t x = 0; x < 16; x++) for (uint32_t y = 0; y < 16; y++)
        {
            // set to revealed
            designations[x][y].bits.hidden = 0;
            // and visible
            designations[x][y].bits.pile = 1;
        }
    }
    out.print("Local map revealed.\n");
}

static void cache_tiles(const df::coord_path & tiles, std::unordered_set<df::coord> & trigger_cache)
{
    size_t num_tiles = tiles.size();
    for (size_t idx = 0; idx < num_tiles; ++idx)
    {
        df::coord pos = tiles[idx];
        pos.x >>= 4;
        pos.y >>= 4;
        trigger_cache.insert(pos);
    }
}

static void initialize_trigger_cache(std::unordered_set<df::coord> & trigger_cache)
{
    for (auto & horror : world->encased_horrors)
        cache_tiles(horror->tiles, trigger_cache);
    for (auto & hollow : world->deep_vein_hollows)
        cache_tiles(hollow->tiles, trigger_cache);
    for (auto & treasure : world->divine_treasures)
        cache_tiles(treasure->tiles, trigger_cache);
}

command_result reveal(color_ostream &out, vector<string> & params)
{
    bool no_hell = true;
    bool pause = true;
    for(size_t i = 0; i < params.size();i++)
    {
        if(params[i]=="hell")
            no_hell = false;
        else if(params[i] == "help" || params[i] == "?")
            return CR_WRONG_USAGE;
    }
    auto& con = out;
    if(params.size() && params[0] == "hell")
    {
        no_hell = false;
    }
    if(params.size() && params[0] == "demon")
    {
        con.printerr("`reveal demon` is currently disabled to prevent a hang due to a bug in the base game\n");
        return CR_FAILURE;
        //no_hell = false;
        //pause = false;
    }
    if(revealed != NOT_REVEALED)
    {
        con.printerr("Map is already revealed or this is a different map.\n");
        return CR_FAILURE;
    }

    CoreSuspender suspend;

    if (!Maps::IsValid())
    {
        out.printerr("Map is not available!\n");
        return CR_FAILURE;
    }

    size_t initial_buckets = 2 * (world->encased_horrors.size() + world->divine_treasures.size() + world->deep_vein_hollows.size());
    std::unordered_set<df::coord> trigger_cache(initial_buckets);
    initialize_trigger_cache(trigger_cache);

    t_gamemodes gm;
    World::ReadGameMode(gm);
    if(gm.g_mode == game_mode::ADVENTURE)
    {
        revealAdventure(out, trigger_cache);
        return CR_OK;
    }
    if(gm.g_mode != game_mode::DWARF)
    {
        con.printerr("Only in fortress mode.\n");
        return CR_FAILURE;
    }

    Maps::getSize(x_max,y_max,z_max);
    hidesaved.reserve(world->map.map_blocks.size());
    for (size_t i = 0; i < world->map.map_blocks.size(); i++)
    {
        df::map_block *block = world->map.map_blocks[i];
        // in 'no-hell'/'safe' mode, don't reveal blocks with hell and adamantine
        if (no_hell && !isSafe(block->map_pos, trigger_cache))
            continue;
        hideblock hb;
        hb.c = block->map_pos;
        designations40d & designations = block->designation;
        // for each tile in block
        for (uint32_t x = 0; x < 16; x++) for (uint32_t y = 0; y < 16; y++)
        {
            // save hidden state of tile
            hb.hiddens[x][y] = designations[x][y].bits.hidden;
            // set to revealed
            designations[x][y].bits.hidden = 0;
        }
        hidesaved.push_back(hb);
    }
    if(no_hell)
    {
        revealed = SAFE_REVEALED;
    }
    else
    {
        if(pause)
        {
            revealed = REVEALED;
            World::SetPauseState(true);
        }
        else
            revealed = DEMON_REVEALED;
    }
    is_active = nopause_state || (revealed == REVEALED);
    bool graphics_mode = Screen::inGraphicsMode();
    con.print("Map revealed.\n\n");
    if (graphics_mode) {
        con.print("Note that in graphics mode, tiles that are not adjacent to open\n"
                  "space will not render but can still be examined by hovering over\n"
                  "them with the mouse. Switching to text mode (in the game settings)\n"
                  "will allow the display of the revealed tiles.\n\n");
    }
    if(!no_hell)
        con.print("Unpausing can unleash the forces of hell, so it has been temporarily disabled.\n\n");
    con.print("Run 'unreveal' to revert to previous state.\n");
    return CR_OK;
}

command_result unreveal(color_ostream &out, vector<string> & params)
{
    auto & con = out;
    for(size_t i = 0; i < params.size();i++)
    {
        if(params[i] == "help" || params[i] == "?")
            return CR_WRONG_USAGE;
    }
    if(!revealed)
    {
        con.printerr("There's nothing to revert!\n");
        return CR_FAILURE;
    }
    CoreSuspender suspend;

    if (!Maps::IsValid())
    {
        out.printerr("Map is not available!\n");
        return CR_FAILURE;
    }
    t_gamemodes gm;
    World::ReadGameMode(gm);
    if(gm.g_mode != game_mode::DWARF)
    {
        con.printerr("Only in fortress mode.\n");
        return CR_FAILURE;
    }

    // Sanity check: map size
    uint32_t x_max_b, y_max_b, z_max_b;
    Maps::getSize(x_max_b,y_max_b,z_max_b);
    if(x_max != x_max_b || y_max != y_max_b || z_max != z_max_b)
    {
        con.printerr("The map is not of the same size...\n");
        return CR_FAILURE;
    }

    for(size_t i = 0; i < hidesaved.size();i++)
    {
        hideblock & hb = hidesaved[i];
        df::map_block * b = Maps::getTileBlock(hb.c.x,hb.c.y,hb.c.z);
        for (uint32_t x = 0; x < 16;x++) for (uint32_t y = 0; y < 16;y++)
        {
            b->designation[x][y].bits.hidden = hb.hiddens[x][y];
        }
    }
    // give back memory.
    hidesaved.clear();
    revealed = NOT_REVEALED;
    is_active = nopause_state || (revealed == REVEALED);
    con.print("Map hidden!\n");
    return CR_OK;
}

command_result revtoggle (color_ostream &out, vector<string> & params)
{
    for(size_t i = 0; i < params.size();i++)
    {
        if(params[i] == "help" || params[i] == "?")
        {
            out.print("Toggles between reveal and unreveal.\nCurrently it: ");
            break;
        }
    }
    if(revealed)
    {
        return unreveal(out,params);
    }
    else
    {
        return reveal(out,params);
    }
}

// Unhides map tiles according to visibility, starting from the given
// coordinates. This algorithm only processes adjacent hidden tiles, so it must
// start on a hidden tile and it will not reveal hidden sections separated by
// already-unhidden tiles.
static void unhideFlood_internal(MapCache *MCache, const DFCoord &xy)
{
    typedef std::pair <DFCoord, bool> foo;
    std::stack < foo > flood;
    flood.push( foo(xy,false) );

    while( !flood.empty() )
    {
        foo tile = flood.top();
        DFCoord & current = tile.first;
        bool & from_below = tile.second;
        flood.pop();

        if(!MCache->testCoord(current))
            continue;
        df::tile_designation des = MCache->designationAt(current);
        if(!des.bits.hidden)
        {
            continue;
        }

        // we don't want constructions or ice to restrict vision (to avoid bug #1871)
        // so use the base tile beneath it
        df::tiletype tt = MCache->baseTiletypeAt(current);

        // UNLESS the actual tile has more visibility than the base
        // i.e. if it's a downward or up/down stairway
        df::tiletype ctt = MCache->tiletypeAt(current);
        switch (tileShape(ctt))
        {
        case tiletype_shape::STAIR_UPDOWN:
        case tiletype_shape::STAIR_DOWN:
            tt = ctt;
            break;
        default:
            break;
        }

        bool below = false;
        bool above = false;
        bool sides = false;
        bool unhide = true;
        // By tile shape, determine behavior and action
        switch (tileShape(tt))
        {
        // Walls
        case tiletype_shape::WALL:
            if (from_below)
                unhide = false;
            break;
        // Open space
        case tiletype_shape::NONE:
        case tiletype_shape::EMPTY:
        case tiletype_shape::RAMP_TOP:
        case tiletype_shape::STAIR_UPDOWN:
        case tiletype_shape::STAIR_DOWN:
        case tiletype_shape::BROOK_TOP:
            above = below = sides = true;
            break;
        // Floors
        case tiletype_shape::FORTIFICATION:
        case tiletype_shape::STAIR_UP:
        case tiletype_shape::RAMP:
        case tiletype_shape::FLOOR:
        case tiletype_shape::BRANCH:
        case tiletype_shape::TRUNK_BRANCH:
        case tiletype_shape::TWIG:
        case tiletype_shape::SAPLING:
        case tiletype_shape::SHRUB:
        case tiletype_shape::BOULDER:
        case tiletype_shape::PEBBLES:
        case tiletype_shape::BROOK_BED:
        case tiletype_shape::ENDLESS_PIT:
            if (from_below)
                unhide = false;
            else
                above = sides = true;
            break;
        }
        // Special case for trees - always reveal them as if they were floor tiles
        if (tileMaterial(tt) == tiletype_material::PLANT || tileMaterial(tt) == tiletype_material::MUSHROOM)
        {
            if (from_below)
                unhide = false;
            else
                above = sides = true;
        }
        if (unhide)
        {
            des.bits.hidden = false;
            MCache->setDesignationAt(current, des);
        }
        if (sides)
        {
            // Scan adjacent tiles clockwise, starting toward east
            flood.push(foo(DFCoord(current.x + 1, current.y    , current.z), false));
            flood.push(foo(DFCoord(current.x + 1, current.y + 1, current.z), false));
            flood.push(foo(DFCoord(current.x    , current.y + 1, current.z), false));
            flood.push(foo(DFCoord(current.x - 1, current.y + 1, current.z), false));
            flood.push(foo(DFCoord(current.x - 1, current.y    , current.z), false));
            flood.push(foo(DFCoord(current.x - 1, current.y - 1, current.z), false));
            flood.push(foo(DFCoord(current.x    , current.y - 1, current.z), false));
            flood.push(foo(DFCoord(current.x + 1, current.y - 1, current.z), false));
        }
        if (above)
        {
            flood.push(foo(DFCoord(current.x, current.y, current.z + 1), true));
        }
        if (below)
        {
            flood.push(foo(DFCoord(current.x, current.y, current.z - 1), false));
        }
    }
}

// Lua entrypoint for unhideFlood_internal
static void unhideFlood(DFCoord pos)
{
    MapCache MCache;
    // no environment or bounds checking needed. if anything is invalid,
    // unhideFlood_internal will just exit immeditately
    unhideFlood_internal(&MCache, pos);
    MCache.WriteAll();
}

command_result revflood(color_ostream &out, vector<string> & params)
{
    for(size_t i = 0; i < params.size();i++)
    {
        if(params[i] == "help" || params[i] == "?")
            return CR_WRONG_USAGE;
    }
    CoreSuspender suspend;
    uint32_t x_max,y_max,z_max;
    if (!Maps::IsValid())
    {
        out.printerr("Map is not available!\n");
        return CR_FAILURE;
    }
    if(revealed != NOT_REVEALED)
    {
        out.printerr("This is only safe to use with non-revealed map.\n");
        return CR_FAILURE;
    }
    t_gamemodes gm;
    World::ReadGameMode(gm);
    if(!World::isFortressMode(gm.g_type) || gm.g_mode != game_mode::DWARF )
    {
        out.printerr("Only in proper dwarf mode.\n");
        return CR_FAILURE;
    }
    df::coord pos;
    Maps::getSize(x_max,y_max,z_max);

    Gui::getCursorCoords(pos);
    if (!pos.isValid()) {
        df::unit *unit = Gui::getSelectedUnit(out, true);
        if (unit)
            pos = Units::getPosition(unit);
    }

    if (!pos.isValid()) {
        vector<df::unit *> citizens;
        Units::getCitizens(citizens);
        if (citizens.size())
            pos = Units::getPosition(citizens[0]);
    }

    if(!pos.isValid()) {
        out.printerr("Please select a unit or place the keyboard cursor at some empty space you want to be unhidden.\n");
        return CR_FAILURE;
    }
    MapCache * MCache = new MapCache;
    df::tiletype tt = MCache->tiletypeAt(pos);
    if(isWallTerrain(tt))
    {
        out.printerr("Please select a unit or place the keyboard cursor at some empty space you want to be unhidden.\n");
        delete MCache;
        return CR_FAILURE;
    }
    // hide all tiles, flush cache
    Maps::getSize(x_max,y_max,z_max);

    for(size_t i = 0; i < world->map.map_blocks.size(); i++)
    {
        df::map_block * b = world->map.map_blocks[i];
        // change the hidden flag to 0
        for (uint32_t x = 0; x < 16; x++) for (uint32_t y = 0; y < 16; y++)
        {
            b->designation[x][y].bits.hidden = 1;
        }
    }
    MCache->trash();

    unhideFlood_internal(MCache, pos);
    MCache->WriteAll();
    delete MCache;

    return CR_OK;
}

command_result revforget(color_ostream &out, vector<string> & params)
{
    auto & con = out;
    for(size_t i = 0; i < params.size();i++)
    {
        if(params[i] == "help" || params[i] == "?")
            return CR_WRONG_USAGE;
    }
    if(!revealed)
    {
        con.printerr("There's nothing to forget!\n");
        return CR_FAILURE;
    }
    // give back memory.
    hidesaved.clear();
    revealed = NOT_REVEALED;
    is_active = nopause_state || (revealed == REVEALED);
    con.print("Reveal data forgotten!\n");
    return CR_OK;
}

DFHACK_PLUGIN_LUA_FUNCTIONS {
    DFHACK_LUA_FUNCTION(unhideFlood),
    DFHACK_LUA_END
};
