#include "inventory_ui.h"

#include "game.h"
#include "player.h"
#include "action.h"
#include "map.h"
#include "translations.h"
#include "options.h"
#include "messages.h"
#include "catacharset.h"
#include "vehicle.h"
#include "vehicle_selector.h"
#include "cata_utility.h"
#include "item.h"
#include "itype.h"

#include <string>
#include <vector>
#include <map>
#include <numeric>
#include <sstream>
#include <algorithm>

/** The maximum distance from the screen edge, to snap a window to it */
static const size_t max_win_snap_distance = 4;
/** The minimal gap between two cells */
static const int min_cell_gap = 2;
/** The gap between two cells when screen space is limited*/
static const int normal_cell_gap = 4;
/** The minimal gap between the first cell and denial */
static const int min_denial_gap = 2;
/** The minimal gap between two columns */
static const int min_column_gap = 2;
/** The gap between two columns when there's enough space, but they are not centered */
static const int normal_column_gap = 8;
/** The minimal occupancy ratio (see @refer get_columns_occupancy_ratio()) to align columns to the center */
static const double min_ratio_to_center = 0.65;

struct navigation_mode_data {
    navigation_mode next_mode;
    std::string name;
    nc_color color;
};

struct inventory_input
{
    std::string action;
    long ch;
    inventory_entry *entry;
};

bool inventory_entry::operator==( const inventory_entry &other ) const {
    return get_category_ptr() == other.get_category_ptr() && location == other.location;
}

class selection_column_preset: public inventory_selector_preset
{
    public:
        selection_column_preset() {}

        virtual std::string get_caption( const inventory_entry &entry ) const override {
            std::ostringstream res;
            const size_t available_count = entry.get_available_count();
            if( entry.chosen_count > 0 && entry.chosen_count < available_count ) {
                res << string_format( _( "%d of %d" ), entry.chosen_count, available_count ) << ' ';
            } else if( available_count != 1 ) {
                res << available_count << ' ';
            }
            res << entry.location->display_name( available_count );
            return res.str();
        }

        virtual nc_color get_color( const inventory_entry &entry ) const override {
            if( &*entry.location == &g->u.weapon ) {
                return c_ltblue;
            } else if( g->u.is_worn( *entry.location ) ) {
                return c_cyan;
            } else {
                return inventory_selector_preset::get_color( entry );
            }
        }
};

static const selection_column_preset selection_preset;

size_t inventory_entry::get_available_count() const {
    if( location && stack_size == 1 ) {
        return location->count_by_charges() ? location->charges : 1;
    } else {
        return stack_size;
    }
}

long inventory_entry::get_invlet() const {
    if( custom_invlet != LONG_MIN ) {
        return custom_invlet;
    }
    return location ? location->invlet : '\0';
}

nc_color inventory_entry::get_invlet_color() const
{
    if( !is_selectable() ) {
        return c_dkgray;
    } else if( g->u.assigned_invlet.count( get_invlet() ) ) {
        return c_yellow;
    } else {
        return c_white;
    }
}

const item_category *inventory_entry::get_category_ptr() const {
    if( custom_category != nullptr ) {
        return custom_category;
    }
    return location ? &location->get_category() : nullptr;
}

bool inventory_column::activatable() const
{
    return std::any_of( entries.begin(), entries.end(), []( const inventory_entry &e ) {
        return e.is_selectable();
    } );
}

inventory_entry *inventory_column::find_by_invlet( long invlet ) const
{
    for( const auto &elem : entries ) {
        if( elem.is_item() && elem.get_invlet() == invlet ) {
            return const_cast<inventory_entry *>( &elem );
        }
    }
    return nullptr;
}

size_t inventory_column::get_width() const
{
    return std::max( get_cells_width(), reserved_width );
}

size_t inventory_column::get_height() const {
    return std::min( entries.size(), entries_per_page );
}

inventory_selector_preset::inventory_selector_preset()
{
    append_cell(
        std::function<std::string( const inventory_entry & )>([ this ]( const inventory_entry & entry ) {
            return get_caption( entry );
    } ) );
}

bool inventory_selector_preset::sort_compare( const item_location &lhs, const item_location &rhs ) const
{
    return lhs->tname( 1 ).compare( rhs->tname( 1 ) ) < 0; // Simple alphabetic order
}

nc_color inventory_selector_preset::get_color( const inventory_entry &entry ) const
{
    return entry.is_item() ? entry.location->color_in_inventory() : c_magenta;
}

std::string inventory_selector_preset::get_caption( const inventory_entry &entry ) const
{
    const size_t count = entry.get_stack_size();
    const std::string disp_name = entry.location->display_name( count );

    return ( count > 1 ) ? string_format( "%d %s", count, disp_name.c_str() ) : disp_name;
}

std::string inventory_selector_preset::get_cell_text( const inventory_entry &entry,
        size_t cell_index ) const
{
    if( cell_index >= cells.size() ) {
        debugmsg( "Invalid cell index %d.", cell_index );
        return "it's a bug!";
    }
    if( !entry ) {
        return std::string();
    } else if( entry.is_item() ) {
        return replace_colors( cells[cell_index].func( entry ) );
    } else if( cell_index != 0 ) {
        return replace_colors( cells[cell_index].title );
    } else {
        return entry.get_category_ptr()->name;
    }
}

size_t inventory_selector_preset::get_cell_width( const inventory_entry &entry, size_t cell_index ) const
{
    return utf8_width( get_cell_text( entry, cell_index ), true );
}

bool inventory_selector_preset::is_stub_cell( const inventory_entry &entry, size_t cell_index ) const
{
    if( !entry.is_item() ) {
        return false;
    }
    const std::string &text = get_cell_text( entry, cell_index );
    return text.empty() || text == cells[cell_index].stub;
}

void inventory_selector_preset::append_cell( const std::function<std::string( const item_location & )> &func,
                                             const std::string &title, const std::string &stub )
{
    // Don't capture by reference here. The func should be able to die earlier than the object itself
    append_cell( std::function<std::string( const inventory_entry & )>( [ func ]( const inventory_entry & entry ) {
        return func( entry.location );
    } ), title, stub );
}

void inventory_selector_preset::append_cell( const std::function<std::string( const inventory_entry & )> &func,
                                             const std::string &title, const std::string &stub )
{
    const auto iter = std::find_if( cells.begin(), cells.end(), [ &title ]( const cell_t & cell ) {
        return cell.title == title;
    } );
    if( iter != cells.end() ) {
        debugmsg( "Tried to append a duplicate cell \"%s\": ignored.", title.c_str() );
        return;
    }
    cells.emplace_back( func, title, stub );
}

void inventory_column::select( size_t new_index, scroll_direction dir )
{
    if( new_index < entries.size() ) {
        if( !entries[new_index].is_selectable() ) {
            new_index = next_selectable_index( new_index, dir );
        }

        selected_index = new_index;
        page_offset = selected_index - selected_index % entries_per_page;
    }
}

size_t inventory_column::next_selectable_index( size_t index, scroll_direction dir ) const
{
    if( entries.empty() ) {
        return index;
    }

    size_t new_index = index;
    do {
        // 'new_index' incremented by 'dir' using division remainder (number of entries) to loop over the entries.
        // Negative step '-k' (backwards) is equivalent to '-k + N' (forward), where:
        //     N = entries.size()  - number of elements,
        //     k = |step|          - absolute step (k <= N).
        new_index = ( new_index + int( dir ) + entries.size() ) % entries.size();
    } while( new_index != index && !entries[new_index].is_selectable() );

    return new_index;
}

void inventory_column::move_selection( scroll_direction dir )
{
    size_t index = selected_index;

    do {
        index = next_selectable_index( index, dir );
    } while( index != selected_index && is_selected_by_category( entries[index] ) );

    select( index, dir );
}

void inventory_column::move_selection_page( scroll_direction dir )
{
    size_t index = selected_index;

    do {
        const size_t next_index = next_selectable_index( index, dir );
        const bool flipped = next_index == selected_index || ( next_index > selected_index ) != ( int( dir ) > 0 );

        if( flipped && page_of( next_index ) == page_index() ) {
            break; // If flipped and still on the same page - no need to flip
        }

        index = next_index;
    } while( page_of( next_selectable_index( index, dir ) ) == page_index() );

    select( index, dir );
}

size_t inventory_column::get_entry_cell_width( const inventory_entry &entry, size_t cell_index ) const
{
    size_t res = preset.get_cell_width( entry, cell_index );

    if( cell_index == 0 ) {
        res += get_entry_indent( entry );    // The indentation always persist
    }

    return res;
}

size_t inventory_column::get_cells_width() const
{
    return std::accumulate( cells.begin(), cells.end(), size_t( 0 ), []( size_t lhs, const cell_t &cell ) {
        return lhs + cell.current_width;
    } );
}

std::string inventory_column::get_entry_denial( const inventory_entry &entry ) const
{
    return entry.is_item() ? preset.get_denial( entry.location ) : std::string();
}

void inventory_column::set_width( const size_t width )
{
    reset_width();
    int width_gap = get_width() - width;
    // Now adjust the width if we must
    while( width_gap != 0 ) {
        const int step = width_gap > 0 ? -1 : 1;
        // Should return true when lhs < rhs
        const auto cmp_for_expansion = []( const cell_t &lhs, const cell_t &rhs ) {
            return lhs.visible() && lhs.gap() < rhs.gap();
        };
        // Should return true when lhs < rhs
        const auto cmp_for_shrinking = []( const cell_t &lhs, const cell_t &rhs ) {
            if( !lhs.visible() ) {
                return false;
            }
            if( rhs.gap() <= min_cell_gap ) {
                return lhs.current_width < rhs.current_width;
            } else {
                return lhs.gap() < rhs.gap();
            }
        };

        const auto &cell = step > 0
            ? std::min_element( cells.begin(), cells.end(), cmp_for_expansion )
            : std::max_element( cells.begin(), cells.end(), cmp_for_shrinking );

        if( cell == cells.end() || !cell->visible() ) {
            break; // This is highly unlikely to happen, but just in case
        }

        cell->current_width += step;
        width_gap += step;
    }
    reserved_width = width;
}

void inventory_column::set_height( size_t height ) {
    if( entries_per_page != height ) {
        if( height == 0 ) {
            debugmsg( "Unable to assign zero height." );
            return;
        }
        entries_per_page = height;
        paging_is_valid = false;
    }
}

void inventory_column::expand_to_fit( const inventory_entry &entry )
{
    if( !entry ) {
        return;
    }

    const std::string denial = get_entry_denial( entry );

    for( size_t i = 0, num = denial.empty() ? cells.size() : 1; i < num; ++i ) {
        auto &cell = cells[i];

        cell.real_width = std::max( cell.real_width, get_entry_cell_width( entry, i ) );

        // Don't reveal the cell for headers and stubs
        if( cell.visible() || ( entry.is_item() && !preset.is_stub_cell( entry, i ) ) ) {
            const size_t cell_gap = i > 0 ? normal_cell_gap : 0;
            cell.current_width = std::max( cell.current_width, cell_gap + cell.real_width );
        }
    }

    if( !denial.empty() ) {
        reserved_width = std::max( get_entry_cell_width( entry, 0 ) + min_denial_gap + utf8_width( denial, true ),
                                   reserved_width );
    }
}

void inventory_column::reset_width()
{
    for( auto &elem : cells ) {
        elem = cell_t();
    }
    reserved_width = 0;
    for( auto &elem : entries ) {
        expand_to_fit( elem );
    }
}

size_t inventory_column::page_of( size_t index ) const {
    return index / entries_per_page;
}

size_t inventory_column::page_of( const inventory_entry &entry ) const {
    return page_of( std::distance( entries.begin(), std::find( entries.begin(), entries.end(), entry ) ) );
}

bool inventory_column::is_selected( const inventory_entry &entry ) const
{
    return entry == get_selected() || ( multiselect && is_selected_by_category( entry ) );
}

bool inventory_column::is_selected_by_category( const inventory_entry &entry ) const
{
    return entry.is_item() && mode == navigation_mode::CATEGORY
                           && entry.get_category_ptr() == get_selected().get_category_ptr()
                           && page_of( entry ) == page_index();
}

const inventory_entry &inventory_column::get_selected() const {
    if( selected_index >= entries.size() || !entries[selected_index].is_item() ) {
        static const inventory_entry dummy;
        return dummy;
    }
    return entries[selected_index];
}

std::vector<inventory_entry *> inventory_column::get_all_selected() const
{
    std::vector<inventory_entry *> res;

    if( allows_selecting() ) {
        for( const auto &elem : entries ) {
            if( is_selected( elem ) ) {
                res.push_back( const_cast<inventory_entry *>( &elem ) );
            }
        }
    }

    return res;
}

void inventory_column::on_input( const inventory_input &input )
{
    if( empty() || !active ) {
        return; // ignore
    }

    if( input.action == "DOWN" ) {
        move_selection( scroll_direction::FORWARD );
    } else if( input.action == "UP" ) {
        move_selection( scroll_direction::BACKWARD );
    } else if( input.action == "NEXT_TAB" ) {
        move_selection_page( scroll_direction::FORWARD );
    } else if( input.action == "PREV_TAB" ) {
        move_selection_page( scroll_direction::BACKWARD );
    } else if( input.action == "HOME" ) {
        select( 0, scroll_direction::FORWARD );
    } else if( input.action == "END" ) {
        select( entries.size() - 1, scroll_direction::BACKWARD );
    }
}

void inventory_column::add_entry( const inventory_entry &entry )
{
    if( std::find( entries.begin(), entries.end(), entry ) != entries.end() ) {
        debugmsg( "Tried to add a duplicate entry." );
        return;
    }
    const auto iter = std::find_if( entries.rbegin(), entries.rend(),
    [ &entry ]( const inventory_entry & cur ) {
        const item_category *cur_cat = cur.get_category_ptr();
        const item_category *new_cat = entry.get_category_ptr();

        return cur_cat == new_cat || ( cur_cat != nullptr && new_cat != nullptr
                                    && cur_cat->sort_rank <= new_cat->sort_rank );
    } );
    entries.insert( iter.base(), entry );
    expand_to_fit( entry );
    paging_is_valid = false;
}

void inventory_column::move_entries_to( inventory_column &dest )
{
    for( const auto &elem : entries ) {
        if( elem.is_item() ) {
            dest.add_entry( elem );
        }
    }
    dest.prepare_paging();
    clear();
}

void inventory_column::prepare_paging()
{
    if( paging_is_valid ) {
        return;
    }
    // First, remove all non-items
    const auto new_end = std::remove_if( entries.begin(), entries.end(), []( const inventory_entry &entry ) {
        return !entry.is_item();
    } );
    entries.erase( new_end, entries.end() );
    // Then sort them with respect to categories
    auto from = entries.begin();
    while( from != entries.end() ) {
        auto to = std::next( from );
        while( to != entries.end() && from->get_category_ptr() == to->get_category_ptr() ) {
            std::advance( to, 1 );
        }
        std::sort( from, to, [ this ]( const inventory_entry &lhs, const inventory_entry &rhs ) {
            if( lhs.is_selectable() != rhs.is_selectable() ) {
                return lhs.is_selectable(); // Disabled items always go last
            }
            return preset.sort_compare( lhs.location, rhs.location );
        } );
        from = to;
    }
    // Recover categories according to the new number of entries per page
    const item_category *current_category = nullptr;
    for( size_t i = 0; i < entries.size(); ++i ) {
        if( entries[i].get_category_ptr() == current_category && i % entries_per_page != 0 ) {
            continue;
        }
        current_category = entries[i].get_category_ptr();
        const inventory_entry insertion = ( i % entries_per_page == entries_per_page - 1 )
            ? inventory_entry() // the last item on the page must not be a category
            : inventory_entry( current_category ); // the first item on the page must be a category
        entries.insert( entries.begin() + i, insertion );
        expand_to_fit( insertion );
    }

    paging_is_valid = true;
    // Select the uppermost possible entry
    select( 0, scroll_direction::FORWARD );
}

void inventory_column::remove_entry( const inventory_entry &entry )
{
    const auto iter = std::find( entries.begin(), entries.end(), entry );
    if( iter == entries.end() ) {
        debugmsg( "Tried to remove a non-existing entry." );
        return;
    }
    entries.erase( iter );
    paging_is_valid = false;
}

void inventory_column::clear()
{
    entries.clear();
    paging_is_valid = false;
    prepare_paging();
}

size_t inventory_column::get_entry_indent( const inventory_entry &entry ) const {
    if( !entry.is_item() ) {
        return 0;
    }

    size_t res = 2;
    if( get_option<bool>("ITEM_SYMBOLS" ) ) {
        res += 2;
    }
    if( allows_selecting() && multiselect ) {
        res += 2;
    }
    return res;
}

long inventory_column::reassign_custom_invlets( const player &p, long min_invlet, long max_invlet )
{
    long cur_invlet = min_invlet;
    for( auto &elem : entries ) {
        // Only items on map/in vehicles: those that the player does not possess.
        if( elem.is_selectable() && !p.has_item( *elem.location ) ) {
            elem.custom_invlet = cur_invlet <= max_invlet ? cur_invlet++ : '\0';
        }
    }
    return cur_invlet;
}

void inventory_column::draw( WINDOW *win, size_t x, size_t y ) const
{
    if( !visible() ) {
        return;
    }

    const auto available_cell_width = [ this ]( const inventory_entry &entry, const size_t cell_index ) {
        const size_t displayed_width = cells[cell_index].current_width;
        const size_t real_width = get_entry_cell_width( entry, cell_index );

        return displayed_width > real_width ? displayed_width - real_width : 0;
    };

    // Do the actual drawing
    for( size_t index = page_offset, line = 0; index < entries.size() && line < entries_per_page; ++index, ++line ) {
        const auto &entry = entries[index];

        if( !entry ) {
            continue;
        }

        int x1 = x + get_entry_indent( entry );
        int x2 = x + std::max( int( reserved_width - get_cells_width() ), 0 );
        int yy = y + line;

        const bool selected = active && is_selected( entry );

        if( selected && visible_cells() > 1 ) {
            for( int hx = x1, hx_max = x + get_width(); hx < hx_max; ++hx ) {
                mvwputch( win, yy, hx, h_white, ' ' );
            }
        }

        const std::string &denial = get_entry_denial( entry );

        if( !denial.empty() ) {
            const size_t max_denial_width = std::max( int( get_width() - ( min_denial_gap + get_entry_cell_width( entry, 0 ) ) ), 0 );
            const size_t denial_width = std::min( max_denial_width, size_t( utf8_width( denial, true ) ) );

            trim_and_print( win, yy, x + get_width() - denial_width, denial_width, c_red, "%s", denial.c_str() );
        }

        const size_t count = denial.empty() ? cells.size() : 1;

        for( size_t cell_index = 0; cell_index < count; ++cell_index ) {
            if( !cells[cell_index].visible() ) {
                continue; // Don't show empty cells
            }

            if( line != 0 && cell_index != 0 && entry.is_category() ) {
                break; // Don't show duplicated titles
            }

            x2 += cells[cell_index].current_width;

            size_t text_width = preset.get_cell_width( entry, cell_index );
            size_t text_gap = cell_index > 0 ? std::max( cells[cell_index].gap(), min_cell_gap ) : 0;
            size_t available_width = x2 - x1 - text_gap;

            if( text_width > available_width ) {
                // See if we can steal some of the needed width from an adjacent cell
                if( cell_index == 0 && count >= 2 ) {
                    available_width += available_cell_width( entry, 1 );
                } else if( cell_index > 0 ) {
                    available_width += available_cell_width( entry, cell_index - 1 );
                }
                text_width = std::min( text_width, available_width );
            }

            if( text_width > 0 ) {
                const int text_x = cell_index == 0 ? x1 : x2 - text_width; // Align either to the left or to the right
                const std::string text = preset.get_cell_text( entry, cell_index );

                if( entry.is_item() && ( selected || !entry.is_selectable() ) ) {
                    trim_and_print( win, yy, text_x, text_width, selected ? h_white : c_dkgray, "%s", remove_color_tags( text ).c_str() );
                } else {
                    trim_and_print( win, yy, text_x, text_width, preset.get_color( entry.location ), "%s", text.c_str() );
                }
            }

            x1 = x2;
        }

        if( entry.is_item() ) {
            int xx = x;
            if( entry.get_invlet() != '\0' ) {
                mvwputch( win, yy, x, entry.get_invlet_color(), entry.get_invlet() );
            }
            xx += 2;
            if( get_option<bool>( "ITEM_SYMBOLS" ) ) {
                const nc_color color = entry.location->color();
                mvwputch( win, yy, xx, color, entry.location->symbol() );
                xx += 2;
            }
            if( allows_selecting() && multiselect ) {
                if( entry.chosen_count == 0 ) {
                    mvwputch( win, yy, xx, c_dkgray, '-' );
                } else if( entry.chosen_count >= entry.get_available_count() ) {
                    mvwputch( win, yy, xx, c_ltgreen, '+' );
                } else {
                    mvwputch( win, yy, xx, c_ltgreen, '#' );
                }
            }
        }
    }
}

size_t inventory_column::visible_cells() const
{
    return std::count_if( cells.begin(), cells.end(), []( const cell_t &elem ) {
        return elem.visible();
    } );
}

selection_column::selection_column( const std::string &id, const std::string &name ) :
    inventory_column( selection_preset ),
    selected_cat( new item_category( id, name, 0 ) ) {}

void selection_column::prepare_paging()
{
    inventory_column::prepare_paging();
    if( entries.empty() ) { // Category must always persist
        entries.emplace_back( selected_cat.get() );
        expand_to_fit( entries.back() );
    }
}

void selection_column::on_change( const inventory_entry &entry )
{
    inventory_entry my_entry( entry, selected_cat.get() );

    const auto iter = std::find( entries.begin(), entries.end(), my_entry );

    if( my_entry.chosen_count != 0 ) {
        if( iter == entries.end() ) {
            add_entry( my_entry );
        } else {
            iter->chosen_count = my_entry.chosen_count;
            expand_to_fit( my_entry );
        }
    } else {
        remove_entry( my_entry );
    }

    prepare_paging();
    // Now let's update selection
    const auto select_iter = std::find( entries.begin(), entries.end(), my_entry );
    if( select_iter != entries.end() ) {
        select( std::distance( entries.begin(), select_iter ), scroll_direction::FORWARD );
    } else {
        select( entries.empty() ? 0 : entries.size() - 1, scroll_direction::BACKWARD ); // Just select the last one
    }
}

// @todo Move it into some 'item_stack' class.
std::vector<std::list<item *>> restack_items( const std::list<item>::const_iterator &from,
                                              const std::list<item>::const_iterator &to )
{
    std::vector<std::list<item *>> res;

    for( auto it = from; it != to; ++it ) {
        auto match = std::find_if( res.begin(), res.end(),
            [ &it ]( const std::list<item *> &e ) {
                return it->stacks_with( *const_cast<item *>( e.back() ) );
            } );

        if( match != res.end() ) {
            match->push_back( const_cast<item *>( &*it ) );
        } else {
            res.emplace_back( 1, const_cast<item *>( &*it ) );
        }
    }

    return res;
}

const item_category *inventory_selector::naturalize_category( const item_category &category, const tripoint &pos )
{
    const auto find_cat_by_id = [ this ]( const std::string &id ) {
        const auto iter = std::find_if( categories.begin(), categories.end(), [ &id ]( const item_category &cat ) {
            return cat.id == id;
        } );
        return iter != categories.end() ? &*iter : nullptr;
    };

    const int dist = rl_dist( u.pos(), pos );

    if( dist != 0 ) {
        const std::string suffix = direction_suffix( u.pos(), pos );
        const std::string id = string_format( "%s_%s", category.id.c_str(), suffix.c_str() );

        const auto existing = find_cat_by_id( id );
        if( existing != nullptr ) {
            return existing;
        }

        const std::string name = string_format( "%s %s", category.name.c_str(), suffix.c_str() );
        const int sort_rank = category.sort_rank + dist;
        const item_category new_category( id, name, sort_rank );

        categories.push_back( new_category );
    } else {
        const auto existing = find_cat_by_id( category.id );
        if( existing != nullptr ) {
            return existing;
        }

        categories.push_back( category );
    }

    return &categories.back();
}

void inventory_selector::add_item( inventory_column &target_column,
                                   const item_location &location,
                                   size_t stack_size, const item_category *custom_category )
{
    if( !preset.is_shown( location ) ) {
        return;
    }

    items.push_back( location.clone() );
    inventory_entry entry( items.back(), stack_size, custom_category,
                           preset.get_denial( location ).empty() );

    target_column.add_entry( entry );
    on_entry_add( entry );

    layout_is_valid = false;
}

void inventory_selector::add_items( inventory_column &target_column,
                                    const std::function<item_location( item * )> &locator,
                                    const std::vector<std::list<item *>> &stacks,
                                    const item_category *custom_category )
{
    const item_category *nat_category = nullptr;

    for( const auto &elem : stacks ) {
        auto loc = locator( elem.front() );

        if( custom_category == nullptr ) {
            nat_category = &loc->get_category();
        } else if( nat_category == nullptr && preset.is_shown( loc ) ) {
            nat_category = naturalize_category( *custom_category, loc.position() );
        }

        add_item( target_column, loc, elem.size(), nat_category );
    }
}

void inventory_selector::add_character_items( Character &character )
{
    static const item_category weapon_held_cat( "WEAPON HELD", _( "WEAPON HELD" ), -200 );
    static const item_category items_worn_cat( "ITEMS WORN", _( "ITEMS WORN" ), -100 );

    character.visit_items( [ this, &character ]( item *it ) {
        if( it == &character.weapon ) {
            add_item( own_gear_column, item_location( character, it ), 1, &weapon_held_cat );
        } else if( character.is_worn( *it ) ) {
            add_item( own_gear_column, item_location( character, it ), 1, &items_worn_cat );
        }
        return VisitResponse::NEXT;
    } );
    // Visitable interface does not support stacks so it has to be here
    for( const auto &elem: character.inv.slice() ) {
        add_item( own_inv_column, item_location( character, &elem->front() ), elem->size() );
    }
}

void inventory_selector::add_map_items( const tripoint &target )
{
    if( g->m.accessible_items( u.pos(), target, rl_dist( u.pos(), target ) ) ) {
        const auto items = g->m.i_at( target );
        const std::string name = to_upper_case( g->m.name( target ) );
        const item_category map_cat( name, name, 100 );

        add_items( map_column, [ &target ]( item * it ) {
            return item_location( target, it );
        }, restack_items( items.begin(), items.end() ), &map_cat );
    }
}

void inventory_selector::add_vehicle_items( const tripoint &target )
{
    int part = -1;
    vehicle *veh = g->m.veh_at( target, part );

    if( veh != nullptr && ( part = veh->part_with_feature( part, "CARGO" ) ) >= 0 ) {
        const auto items = veh->get_items( part );
        const std::string name = to_upper_case( veh->parts[part].name() );
        const item_category vehicle_cat( name, name, 200 );

        add_items( map_column, [ veh, part ]( item *it ) {
            return item_location( vehicle_cursor( *veh, part ), it );
        }, restack_items( items.begin(), items.end() ), &vehicle_cat );
    }
}

void inventory_selector::add_nearby_items( int radius )
{
    if( radius >= 0 ) {
        for( const auto &pos : closest_tripoints_first( radius, u.pos() ) ) {
            add_map_items( pos );
            add_vehicle_items( pos );
        }
    }
}

inventory_entry *inventory_selector::find_entry_by_invlet( long invlet ) const
{
    for( const auto &elem : columns ) {
        const auto res = elem->find_by_invlet( invlet );
        if( res != nullptr ) {
            return res;
        }
    }
    return nullptr;
}

void inventory_selector::rearrange_columns( size_t client_width )
{
    if( !own_gear_column.empty() && is_overflown( client_width ) ) {
        own_gear_column.move_entries_to( own_inv_column );
    }

    if( !map_column.empty() && is_overflown( client_width ) ) {
        map_column.move_entries_to( own_inv_column );
    }
}

void inventory_selector::prepare_layout( size_t client_width, size_t client_height )
{
    // This block adds categories and should go before any width evaluations
    for( auto &elem : columns ) {
        elem->set_height( client_height );
        elem->prepare_paging();
    }
    // Handle screen overflow
    rearrange_columns( client_width );
    // If we have a single column and it occupies more than a half of
    // the available with -> expand it
    auto visible_columns = get_visible_columns();
    if( visible_columns.size() == 1 && are_columns_centered( client_width ) ) {
        visible_columns.front()->set_width( client_width );
    }

    long custom_invlet = '0';
    for( auto &elem : columns ) {
        elem->prepare_paging();
        custom_invlet = elem->reassign_custom_invlets( u, custom_invlet, '9' );
    }

    refresh_active_column();
}

size_t inventory_selector::get_layout_width() const
{
    const size_t min_hud_width = std::max( get_header_min_width(), get_footer_min_width() );
    const auto visible_columns = get_visible_columns();
    const size_t gaps = visible_columns.size() > 1 ? min_column_gap * ( visible_columns.size() - 1 ) : 0;

    return std::max( get_columns_width( visible_columns ) + gaps, min_hud_width );
}

size_t inventory_selector::get_layout_height() const
{
    const auto visible_columns = get_visible_columns();
    // Find and return the highest column's height.
    const auto iter = std::max_element( visible_columns.begin(), visible_columns.end(),
    []( const inventory_column *lhs, const inventory_column *rhs ) {
        return lhs->get_height() < rhs->get_height();
    } );

    return iter != visible_columns.end() ? ( *iter )->get_height() : 1;
}

size_t inventory_selector::get_header_height() const
{
    return display_stats || !hint.empty() ? 2 : 1;
}

size_t inventory_selector::get_header_min_width() const
{
    const size_t titles_width = std::max( utf8_width( title, true ),
                                          utf8_width( hint, true ) );
    size_t stats_width = 0;
    for( const std::string &elem : get_stats() ) {
        stats_width = std::max( size_t( utf8_width( elem, true ) ), stats_width );
    }
    return titles_width + stats_width + ( stats_width != 0 ? 3 : 0 );
}

size_t inventory_selector::get_footer_min_width() const
{
    size_t result = 0;
    navigation_mode m = mode;

    do {
        result = std::max( size_t( utf8_width( get_footer( m ).first, true ) ), result );
        m = get_navigation_data( m ).next_mode;
    } while( m != mode );

    return result;
}

void inventory_selector::draw_header( WINDOW *w ) const
{
    trim_and_print( w, border, border + 1, getmaxx( w ) - 2 * ( border + 1 ), c_white, "%s", title.c_str() );
    trim_and_print( w, border + 1, border + 1, getmaxx( w ) - 2 * ( border + 1 ), c_dkgray, "%s", hint.c_str() );

    mvwhline( w, border + get_header_height(), border, LINE_OXOX, getmaxx( w ) - 2 * border );

    if( display_stats ) {
        size_t y = border;
        for( const std::string &elem : get_stats() ) {
            right_print( w, y++, border + 1, c_dkgray, elem.c_str() );
        }
    }
}

std::vector<std::string> inventory_selector::get_stats() const
{
    // An array of cells for the stat lines. Example: ["Weight (kg)", "10", "/", "20"].
    using stat = std::array<std::string, 4>;
    // Constructs an array of cells to align them later. 'disp_func' is used to represent numeric values.
    const auto disp = []( const std::string &caption, int cur_value, int max_value,
                          const std::function<std::string( int )> disp_func ) -> stat {
        const std::string color = string_from_color( cur_value > max_value ? c_red : c_ltgray );
        return {{ caption,
                  string_format( "<color_%s>%s</color>", color.c_str(), disp_func( cur_value ).c_str() ), "/",
                  string_format( "<color_ltgray>%s</color>", disp_func( max_value ).c_str() )
        }};
    };

    const player &dummy = get_player_for_stats();
    // Stats consist of arrays of cells.
    const size_t num_stats = 2;
    const std::array<stat, num_stats> stats = {{
        disp( string_format( _( "Weight (%s):" ), weight_units() ),
              dummy.weight_carried(),
              dummy.weight_capacity(), []( int w ) {
            return string_format( "%.1f", round_up( convert_weight( w ), 1 ) );
        } ),
        disp( string_format( _( "Volume (%s):" ), volume_units_abbr() ),
              units::to_milliliter( dummy.volume_carried() ),
              units::to_milliliter( dummy.volume_capacity() ), []( int v ) {
            return format_volume( units::from_milliliter( v ) );
        } )
    }};
    // Strams for every stat.
    std::array<std::ostringstream, num_stats> lines;
    std::array<size_t, num_stats> widths;
    // Add first cells and spaces after them.
    for( size_t i = 0; i < stats.size(); ++i ) {
        lines[i] << stats[i][0] << ' ';
    }
    // Now add the rest of the cells and allign them to the right.
    for( size_t j = 1; j < stats.front().size(); ++j ) {
        // Calculate actual cell width for each stat.
        std::transform( stats.begin(), stats.end(), widths.begin(),
        [j]( const stat &elem ) {
            return utf8_width( elem[j], true );
        } );
        // Determine the max width.
        const size_t max_w = *std::max_element( widths.begin(), widths.end() );
        // Align all stats in this cell with spaces.
        for( size_t i = 0; i < stats.size(); ++i ) {
            if( max_w > widths[i] ) {
                lines[i] << std::string( max_w - widths[i], ' ' );
            }
            lines[i] << stats[i][j];
        }
    }
    // Construct the final result.
    std::vector<std::string> result;
    std::transform( lines.begin(), lines.end(), std::back_inserter( result ),
    []( const std::ostringstream &elem ) {
        return elem.str();
    } );

    return result;
}

void inventory_selector::resize_window( int width, int height )
{
    border = width < TERMX || height < TERMY ? 1 : 0;

    const int w = width + ( width + 2 * border <= TERMX ? 2 * border : 0 );
    const int h = height + ( height + 2 * border <= TERMY ? 2 * border : 0 );
    const int x = VIEW_OFFSET_X + ( TERMX - w ) / 2;
    const int y = VIEW_OFFSET_Y + ( TERMY - h ) / 2;

    w_inv.reset( newwin( h, w, y, x ) );
}

void inventory_selector::refresh_window() const
{
    assert( w_inv );

    werase( w_inv.get() );

    draw_header( w_inv.get() );
    draw_columns( w_inv.get() );
    draw_footer( w_inv.get() );

    if( border != 0 ) {
        draw_frame( w_inv.get() );
    }

    wrefresh( w_inv.get() );
}

void inventory_selector::update()
{
    if( layout_is_valid ) {
        refresh_window();
        return;
    }

    const auto snap = []( size_t cur_dim, size_t max_dim ) {
        return cur_dim + 2 * max_win_snap_distance >= max_dim ? max_dim : cur_dim;
    };

    const size_t nc_width = 2;
    const size_t nc_height = get_header_height() + 3;
    // Prepare an initial layout.
    prepare_layout( TERMX, TERMY );
    // Resize the window (possibly snapping to the screen edges).
    resize_window( snap( get_layout_width() + nc_width, TERMX ),
                   snap( get_layout_height() + nc_height, TERMY ) );
    // Adjust to the new size.
    prepare_layout( getmaxx( w_inv.get() ) - nc_width - 2 * border,
                    getmaxy( w_inv.get() ) - nc_height - 2 * border );

    refresh_window();

    layout_is_valid = true;
}

void inventory_selector::draw_columns( WINDOW *w ) const
{
    const auto columns = get_visible_columns();

    const int screen_width = getmaxx( w ) - 2 * ( border + 1 );
    const bool centered = are_columns_centered( screen_width );

    const int free_space = screen_width - get_columns_width( columns );
    const int max_gap = ( columns.size() > 1 ) ? free_space / ( int( columns.size() ) - 1 ) : free_space;
    const int gap = centered ? max_gap : std::min<int>( max_gap, normal_column_gap );
    const int gap_rounding_error = centered && columns.size() > 1
                                       ? free_space % ( columns.size() - 1 ) : 0;

    size_t x = border + 1;
    size_t y = get_header_height() + border + 1;
    size_t active_x = 0;

    for( const auto &elem : columns ) {
        if( &elem == &columns.back() ) {
            x += gap_rounding_error;
        }

        if( !is_active_column( *elem ) ) {
            elem->draw( w, x, y );
        } else {
            active_x = x;
        }

        if( elem->pages_count() > 1 ) {
            mvwprintw( w, getmaxy( w ) - ( border + 1 ), x, _( "Page %d/%d" ),
                       elem->page_index() + 1, elem->pages_count() );
        }

        x += elem->get_width() + gap;
    }

    get_active_column().draw( w, active_x, y );
    if( empty() ) {
        center_print( w, getmaxy( w ) / 2, c_dkgray, _( "Your inventory is empty." ) );
    }
}

void inventory_selector::draw_frame( WINDOW *w ) const
{
    draw_border( w );

    const int y = border + get_header_height();
    mvwhline( w, y, 0, LINE_XXXO, 1 );
    mvwhline( w, y, getmaxx( w ) - border, LINE_XOXX, 1 );
}

std::pair<std::string, nc_color> inventory_selector::get_footer( navigation_mode m ) const
{
    if( has_available_choices() ) {
        //~ %1$s - category name, %2$s, %3$s - key names
        return std::make_pair( string_format( _( "%1$s; %2$s switches mode, %3$s confirms." ),
                                              get_navigation_data( m ).name.c_str(),
                                              ctxt.get_desc( "CATEGORY_SELECTION" ).c_str(),
                                              ctxt.get_desc( "CONFIRM" ).c_str() ),
                               get_navigation_data( m ).color );
    }
    return std::make_pair( _( "There are no available choices." ), i_red );
}

void inventory_selector::draw_footer( WINDOW *w ) const
{
    const auto footer = get_footer( mode );
    center_print( w, getmaxy( w ) - ( border + 1 ), footer.second, "%s", footer.first.c_str() );
}

inventory_selector::inventory_selector( const player &u, const inventory_selector_preset &preset )
    : u(u)
    , preset( preset )
    , ctxt( "INVENTORY" )
    , columns()
    , active_column_index( 0 )
    , mode( navigation_mode::ITEM )
    , own_inv_column( preset )
    , own_gear_column( preset )
    , map_column( preset )
{
    ctxt.register_action( "DOWN", _( "Next item" ) );
    ctxt.register_action( "UP", _( "Previous item" ) );
    ctxt.register_action( "RIGHT", _( "Next column" ) );
    ctxt.register_action( "LEFT", _( "Previous column" ) );
    ctxt.register_action( "CONFIRM", _( "Confirm your selection" ) );
    ctxt.register_action( "QUIT", _( "Cancel" ) );
    ctxt.register_action( "CATEGORY_SELECTION", _( "Switch selection mode" ) );
    ctxt.register_action( "NEXT_TAB", _( "Page down") );
    ctxt.register_action( "PREV_TAB", _( "Page up" ) );
    ctxt.register_action( "HOME", _( "Home" ) );
    ctxt.register_action( "END", _( "End" ) );
    ctxt.register_action( "HELP_KEYBINDINGS" );
    ctxt.register_action( "ANY_INPUT" ); // For invlets

    append_column( own_inv_column );
    append_column( map_column );
    append_column( own_gear_column );
}

bool inventory_selector::empty() const
{
    return items.empty();
}

bool inventory_selector::has_available_choices() const
{
    return std::any_of( items.begin(), items.end(), [ this ]( const item_location &loc ) {
        return preset.get_denial( loc ).empty();
    } );
}

inventory_input inventory_selector::get_input()
{
    inventory_input res;

    res.action = ctxt.handle_input();
    res.ch = ctxt.get_raw_input().get_first_input();
    res.entry = find_entry_by_invlet( res.ch );

    if( res.entry != nullptr && !res.entry->is_selectable() ) {
        res.entry = nullptr;
    }

    return res;
}

void inventory_selector::on_input( const inventory_input &input )
{
    if( input.action == "CATEGORY_SELECTION" ) {
        toggle_navigation_mode();
    } else if( input.action == "LEFT" ) {
        toggle_active_column( scroll_direction::BACKWARD );
    } else if( input.action == "RIGHT" ) {
        toggle_active_column( scroll_direction::FORWARD );
    } else {
        for( auto &elem : columns ) {
            elem->on_input( input );
        }
        refresh_active_column(); // Columns can react to actions by losing their activation capacity
    }
}

void inventory_selector::on_change( const inventory_entry &entry )
{
    for( auto &elem : columns ) {
        elem->on_change( entry );
    }
    refresh_active_column(); // Columns can react to changes by losing their activation capacity
}

std::vector<inventory_column *> inventory_selector::get_visible_columns() const
{
    std::vector<inventory_column *> res( columns.size() );
    const auto iter = std::copy_if( columns.begin(), columns.end(), res.begin(),
    []( const inventory_column *e ) {
        return e->visible();
    } );
    res.resize( std::distance( res.begin(), iter ) );
    return res;
}

inventory_column &inventory_selector::get_column( size_t index ) const
{
    if( index >= columns.size() ) {
        static inventory_column dummy( preset );
        return dummy;
    }
    return *columns[index];
}

void inventory_selector::set_active_column( size_t index )
{
    if( index < columns.size() && index != active_column_index && get_column( index ).activatable() ) {
        get_active_column().on_deactivate();
        active_column_index = index;
        get_active_column().on_activate();
    }
}

size_t inventory_selector::get_columns_width( const std::vector<inventory_column *> &columns ) const
{
    return std::accumulate( columns.begin(), columns.end(), 0,
    []( const size_t &lhs, const inventory_column *column ) {
        return lhs + column->get_width();
    } );
}

double inventory_selector::get_columns_occupancy_ratio( size_t client_width ) const
{
    const auto visible_columns = get_visible_columns();
    const int free_width = client_width - get_columns_width( visible_columns )
        - min_column_gap * std::max( int( visible_columns.size() ) - 1, 0 );
    return 1.0 - double( free_width ) / client_width;
}

bool inventory_selector::are_columns_centered( size_t client_width ) const {
    return get_columns_occupancy_ratio( client_width ) >= min_ratio_to_center;
}

bool inventory_selector::is_overflown( size_t client_width ) const {
    return get_columns_occupancy_ratio( client_width ) > 1.0;
}

void inventory_selector::toggle_active_column( scroll_direction dir )
{
    if( columns.empty() ) {
        return;
    }

    size_t index = active_column_index;

    do {
        switch( dir ) {
            case scroll_direction::FORWARD:
                index = index + 1 < columns.size() ? index + 1 : 0;
                break;
            case scroll_direction::BACKWARD:
                index = index > 0 ? index - 1 : columns.size() - 1;
                break;
        }
    } while( index != active_column_index && !get_column( index ).activatable() );

    set_active_column( index );
}

void inventory_selector::toggle_navigation_mode()
{
    mode = get_navigation_data( mode ).next_mode;
    for( auto &elem : columns ) {
        elem->set_mode( mode );
    }
}

void inventory_selector::append_column( inventory_column &column )
{
    column.set_mode( mode );

    if( columns.empty() ) {
        column.on_activate();
    }

    columns.push_back( &column );
}

const navigation_mode_data &inventory_selector::get_navigation_data( navigation_mode m ) const
{
    static const std::map<navigation_mode, navigation_mode_data> mode_data = {
        { navigation_mode::ITEM,     { navigation_mode::CATEGORY, _( "Item selection mode" ),     c_ltgray } },
        { navigation_mode::CATEGORY, { navigation_mode::ITEM,     _( "Category selection mode" ), h_white  } }
    };

    return mode_data.at( m );
}

item_location inventory_pick_selector::execute()
{
    while( true ) {
        update();

        const inventory_input input = get_input();

        if( input.entry != nullptr ) {
            return input.entry->location.clone();
        } else if( input.action == "QUIT" ) {
            return item_location();
        } else if( input.action == "CONFIRM" ) {
            return get_active_column().get_selected().location.clone();
        } else {
            on_input( input );
        }
    }
}

inventory_multiselector::inventory_multiselector( const player &p,
        const inventory_selector_preset &preset,
        const std::string &selection_column_title ) :
    inventory_selector( p, preset ),
    selection_col( new selection_column( "SELECTION_COLUMN", selection_column_title ) )
{
    ctxt.register_action( "RIGHT", _( "Mark/unmark selected item" ) );

    for( auto &elem : get_all_columns() ) {
        elem->set_multiselect( true );
    }
    append_column( *selection_col );
}

void inventory_multiselector::rearrange_columns( size_t client_width )
{
    selection_col->set_visibility( !is_overflown( client_width ) );
    inventory_selector::rearrange_columns( client_width );
}

void inventory_multiselector::on_entry_add( const inventory_entry &entry )
{
    if( entry.is_item() ) {
        static_cast<selection_column *>( selection_col.get() )->expand_to_fit( entry );
    }
}

inventory_compare_selector::inventory_compare_selector( const player &p ) :
    inventory_multiselector( p, default_preset, _( "ITEMS TO COMPARE" ) ) {}

std::pair<const item *, const item *> inventory_compare_selector::execute()
{
    while( true ) {
        update();

        const inventory_input input = get_input();

        if( input.entry != nullptr ) {
            toggle_entry( input.entry );
        } else if( input.action == "RIGHT" ) {
            const auto selection( get_active_column().get_all_selected() );

            for( auto &elem : selection ) {
                if( elem->chosen_count == 0 || selection.size() == 1 ) {
                    toggle_entry( elem );
                    if( compared.size() == 2 ) {
                        break;
                    }
                }
            }
        } else if( input.action == "CONFIRM" ) {
            popup_getkey( _( "You need two items for comparison.  Use %s to select them." ),
                          ctxt.get_desc( "RIGHT" ).c_str() );
        } else if( input.action == "QUIT" ) {
            return std::make_pair( nullptr, nullptr );
        } else {
            on_input( input );
        }

        if( compared.size() == 2 ) {
            const auto res = std::make_pair( &*compared.back()->location, &*compared.front()->location );
            toggle_entry( compared.back() );
            return res;
        }
    }
}

void inventory_compare_selector::toggle_entry( inventory_entry *entry )
{
    const auto iter = std::find( compared.begin(), compared.end(), entry );

    entry->chosen_count = ( iter == compared.end() ) ? 1 : 0;

    if( entry->chosen_count != 0 ) {
        compared.push_back( entry );
    } else {
        compared.erase( iter );
    }

    on_change( *entry );
}

inventory_drop_selector::inventory_drop_selector( const player &p,
        const inventory_selector_preset &preset ) :
    inventory_multiselector( p, preset, _( "ITEMS TO DROP" ) ) {}

std::list<std::pair<int, int>> inventory_drop_selector::execute()
{
    int count = 0;
    while( true ) {
        update();

        const inventory_input input = get_input();

        if( input.ch >= '0' && input.ch <= '9' ) {
            count = std::min( count, INT_MAX / 10 - 10 );
            count *= 10;
            count += input.ch - '0';
        } else if( input.entry != nullptr ) {
            set_drop_count( *input.entry, count );
            count = 0;
        } else if( input.action == "RIGHT" ) {
            for( const auto &elem : get_active_column().get_all_selected() ) {
                set_drop_count( *elem, count );
            }
            count = 0;
        } else if( input.action == "CONFIRM" ) {
            if( dropping.empty() ) {
                popup_getkey( _( "No items were selected.  Use %s to select them." ),
                              ctxt.get_desc( "RIGHT" ).c_str() );
                continue;
            }
            break;
        } else if( input.action == "QUIT" ) {
            return std::list<std::pair<int, int> >();
        } else {
            on_input( input );
            count = 0;
        }
    }

    std::list<std::pair<int, int>> dropped_pos_and_qty;

    for( auto drop_pair : dropping ) {
        dropped_pos_and_qty.push_back( std::make_pair( u.get_item_position( drop_pair.first ), drop_pair.second ) );
    }

    return dropped_pos_and_qty;
}

void inventory_drop_selector::set_drop_count( inventory_entry &entry, size_t count )
{
    const item *it = &*entry.location;
    const auto iter = dropping.find( it );

    if( count == 0 && iter != dropping.end() ) {
        entry.chosen_count = 0;
        dropping.erase( iter );
    } else {
        entry.chosen_count = ( count == 0 )
            ? entry.get_available_count()
            : std::min( count, entry.get_available_count() );
        dropping[it] = entry.chosen_count;
    }

    on_change( entry );
}

const player &inventory_drop_selector::get_player_for_stats() const
{
    std::map<item *, int> dummy_dropping;

    dummy.reset( new player( u ) );

    for( const auto &elem : dropping ) {
        dummy_dropping[&dummy->i_at( u.get_item_position( elem.first ) )] = elem.second;
    }
    for( auto &elem : dummy_dropping ) {
        if( elem.first->count_by_charges() ) {
            elem.first->mod_charges( -elem.second );
        } else {
            const int pos = dummy->get_item_position( elem.first );
            for( int i = 0; i < elem.second; ++i ) {
                dummy->i_rem( pos );
            }
        }
    }

    return *dummy;
}
