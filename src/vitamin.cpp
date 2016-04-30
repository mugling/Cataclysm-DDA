#include "vitamin.h"

#include <map>

#include "itype.h"
#include "player.h"
#include "mutation.h"
#include "morale.h"
#include "debug.h"

static std::map<vitamin_id, vitamin> vitamins_all;

template<>
bool string_id<vitamin>::is_valid() const
{
    return vitamins_all.count( *this );
}

template<>
const vitamin &string_id<vitamin>::obj() const
{
    const auto found = vitamins_all.find( *this );
    if( found == vitamins_all.end() ) {
        debugmsg( "Tried to get invalid vitamin: %s", c_str() );
        static const vitamin null_vitamin{};
        return null_vitamin;
    }
    return found->second;
}

const std::pair<efftype_id, int> &vitamin::effect( int level ) const
{
    for( const auto &e : deficiency_ ) {
        if( level <= e.first ) {
            return e.second;
        }
    }
    for( const auto &e : excess_ ) {
        if( level >= e.first ) {
            return e.second;
        }
    }
    static std::pair<efftype_id, int> null_effect = { NULL_ID, 1 };
    return null_effect;
}

void vitamin::load_vitamin( JsonObject &jo )
{
    vitamin vit;

    vit.id_ = vitamin_id( jo.get_string( "id" ) );
    vit.name_ = jo.get_string( "name" );
    vit.min_ = jo.get_int( "min" );
    vit.max_ = jo.get_int( "max", 0 );
    vit.rate_ = jo.get_int( "rate", 60 );

    if( vit.rate_ < 0 ) {
        jo.throw_error( "vitamin consumption rate cannot be negative", "rate" );
    }

    using disease = std::pair<int, std::pair<efftype_id, int>>;

    auto def = jo.get_array( "deficiency" );
    while( def.has_more() ) {
        auto e = def.next_array();
        vit.deficiency_.emplace_back( e.get_int( 0 ),
                                      std::make_pair( efftype_id( e.get_string( 1 ) ), e.get_int( 2 ) ) );
    }
    std::sort( vit.deficiency_.begin(), vit.deficiency_.end(),
               []( const disease& lhs, const disease& rhs ) { return lhs.first < rhs.first; } );

    auto exc = jo.get_array( "excess" );
    while( exc.has_more() ) {
        auto e = exc.next_array();
        vit.excess_.emplace_back( e.get_int( 0 ),
                                  std::make_pair( efftype_id( e.get_string( 1 ) ), e.get_int( 2 ) ) );
    }
    std::sort( vit.excess_.rbegin(), vit.excess_.rend(),
               []( const disease& lhs, const disease& rhs ) { return lhs.first < rhs.first; } );

    if( vitamins_all.find( vit.id_ ) != vitamins_all.end() ) {
        jo.throw_error( "parsed vitamin overwrites existing definition", "id" );
    } else {
        vitamins_all[ vit.id_ ] = vit;
        DebugLog( D_INFO, DC_ALL ) << "Loaded vitamin: " << vit.name_;
    }
}

const std::map<vitamin_id, vitamin> &vitamin::all()
{
    return vitamins_all;
}

void vitamin::reset()
{
    vitamins_all.clear();
}

std::map<vitamin_id, int> player::vitamins_from( const itype_id &id ) const
{
    return vitamins_from( item( id ) );
}

std::map<vitamin_id, int> player::vitamins_from( const item &it ) const
{
    std::map<vitamin_id, int> res;

    if( !it.type->comestible ) {
        return res;
    }

    // food to which the player is allergic to never contains any vitamins
    if( allergy_type( it ) != MORALE_NULL ) {
        return res;
    }

    // @todo bionics and mutations can affect vitamin absorption
    for( const auto &e : it.type->comestible->vitamins ) {
        res.emplace( e.first, e.second );
    }

    return res;
}

int player::vitamin_rate( const vitamin_id &vit ) const
{
    int res = vit.obj().rate();

    for( const auto &m : get_mutations() ) {
        const auto &mut = mutation_branch::get( m );
        auto iter = mut.vitamin_rates.find( vit );
        if( iter != mut.vitamin_rates.end() ) {
            res += iter->second;
        }
    }

    return res;
}

int player::vitamin_mod( const vitamin_id &vit, int qty, bool capped )
{
    auto it = vitamin_levels.find( vit );
    if( it == vitamin_levels.end() ) {
        return 0;
    }
    const auto &v = it->first.obj();

    if( qty > 0 ) {
        // accumulations can never occur from food sources
        it->second = std::min( it->second + qty, capped ? 0 : v.max() );

    } else if( qty < 0 ) {
        it->second = std::max( it->second + qty, v.min() );
    }

    auto eff = v.effect( it->second );
    if( !eff.first.is_null() ) {
        // consumption rate may vary so extend effect until next check due for this vitamin
        add_effect( eff.first, ( std::abs( vitamin_rate( vit ) ) * MINUTES( 1 ) ) - get_effect_dur( eff.first ) + 1,
                    num_bp, false, eff.second );
    }

    return it->second;
}

int player::vitamin_get( const vitamin_id &vit ) const
{
    const auto &v = vitamin_levels.find( vit );
    return v != vitamin_levels.end() ? v->second : 0;
}

bool player::vitamin_set( const vitamin_id &vit, int qty )
{
    auto v = vitamin_levels.find( vit );
    if( v == vitamin_levels.end() ) {
        return false;
    }
    vitamin_mod( vit, qty - v->second, false );

    return true;
}
