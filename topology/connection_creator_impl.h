/*
 *  connection_creator_impl.h
 *
 *  This file is part of NEST.
 *
 *  Copyright (C) 2004 The NEST Initiative
 *
 *  NEST is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  NEST is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with NEST.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#ifndef CONNECTION_CREATOR_IMPL_H
#define CONNECTION_CREATOR_IMPL_H

#include "connection_creator.h"

// C++ includes:
#include <vector>

// Includes from librandom:
#include "binomial_randomdev.h"

// Includes from nestkernel:
#include "kernel_manager.h"
#include "nest.h"

namespace nest
{
template < int D >
void
ConnectionCreator::connect( Layer< D >& source, Layer< D >& target, GIDCollectionPTR target_gc )
{
  switch ( type_ )
  {
  case Target_driven:

    target_driven_connect_( source, target, target_gc );
    break;

  case Convergent:

    convergent_connect_( source, target, target_gc );
    break;

  case Divergent:

    divergent_connect_( source, target, target_gc );
    break;

  case Source_driven:

    source_driven_connect_( source, target, target_gc );
    break;

  default:
    throw BadProperty( "Unknown connection type." );
  }
}

template < typename Iterator, int D >
void
ConnectionCreator::connect_to_target_( Iterator from,
  Iterator to,
  Node* tgt_ptr,
  const Position< D >& tgt_pos,
  thread tgt_thread,
  const Layer< D >& source )
{
  librandom::RngPtr rng = get_vp_rng( tgt_thread );

  const std::vector< double > target_pos = tgt_pos.get_vector();

  const bool without_kernel = not kernel_.get();
  for ( Iterator iter = from; iter != to; ++iter )
  {
    if ( ( not allow_autapses_ ) and ( iter->second == tgt_ptr->get_gid() ) )
    {
      continue;
    }

    if ( without_kernel or rng->drand() < kernel_->value( rng, iter->first.get_vector(), target_pos, source ) )
    {
      kernel().connection_manager.connect( iter->second,
        tgt_ptr,
        tgt_thread,
        synapse_model_,
        dummy_param_,
        delay_->value( rng, iter->first.get_vector(), target_pos, source ),
        weight_->value( rng, iter->first.get_vector(), target_pos, source ) );
    }
  }
}

template < int D >
ConnectionCreator::PoolWrapper_< D >::PoolWrapper_()
  : masked_layer_( 0 )
  , positions_( 0 )
{
}

template < int D >
ConnectionCreator::PoolWrapper_< D >::~PoolWrapper_()
{
  if ( masked_layer_ )
  {
    delete masked_layer_;
  }
}

template < int D >
void
ConnectionCreator::PoolWrapper_< D >::define( MaskedLayer< D >* ml )
{
  assert( masked_layer_ == 0 );
  assert( positions_ == 0 );
  assert( ml != 0 );
  masked_layer_ = ml;
}

template < int D >
void
ConnectionCreator::PoolWrapper_< D >::define( std::vector< std::pair< Position< D >, index > >* pos )
{
  assert( masked_layer_ == 0 );
  assert( positions_ == 0 );
  assert( pos != 0 );
  positions_ = pos;
}

template < int D >
typename Ntree< D, index >::masked_iterator
ConnectionCreator::PoolWrapper_< D >::masked_begin( const Position< D >& pos ) const
{
  return masked_layer_->begin( pos );
}

template < int D >
typename Ntree< D, index >::masked_iterator
ConnectionCreator::PoolWrapper_< D >::masked_end() const
{
  return masked_layer_->end();
}

template < int D >
typename std::vector< std::pair< Position< D >, index > >::iterator
ConnectionCreator::PoolWrapper_< D >::begin() const
{
  return positions_->begin();
}

template < int D >
typename std::vector< std::pair< Position< D >, index > >::iterator
ConnectionCreator::PoolWrapper_< D >::end() const
{
  return positions_->end();
}


template < int D >
void
ConnectionCreator::target_driven_connect_( Layer< D >& source, Layer< D >& target, GIDCollectionPTR target_gc )
{
  // Target driven connect
  // For each local target node:
  //  1. Apply Mask to source layer
  //  2. For each source node: Compute probability, draw random number, make
  //     connection conditionally

  // retrieve global positions, either for masked or unmasked pool
  PoolWrapper_< D > pool;
  if ( mask_.get() ) // MaskedLayer will be freed by PoolWrapper d'tor
  {
    pool.define( new MaskedLayer< D >( source, mask_, allow_oversized_ ) );
  }
  else
  {
    pool.define( source.get_global_positions_vector() );
  }

  std::vector< std::shared_ptr< WrappedThreadException > > exceptions_raised_( kernel().vp_manager.get_num_threads() );

// sharing specs on next line commented out because gcc 4.2 cannot handle them
#pragma omp parallel // default(none) shared(source, target, masked_layer,
                     // target_begin, target_end)
  {
    const int thread_id = kernel().vp_manager.get_thread_id();
    try
    {
      GIDCollection::const_iterator target_begin = target_gc->begin();
      GIDCollection::const_iterator target_end = target_gc->end();

      for ( GIDCollection::const_iterator tgt_it = target_begin; tgt_it < target_end; ++tgt_it )
      {
        Node* const tgt = kernel().node_manager.get_node_or_proxy( ( *tgt_it ).gid, thread_id );

        if ( not tgt->is_proxy() )
        {
          const Position< D > target_pos = target.get_position( ( *tgt_it ).lid );

          if ( mask_.get() )
          {
            connect_to_target_(
              pool.masked_begin( target_pos ), pool.masked_end(), tgt, target_pos, thread_id, source );
          }
          else
          {
            connect_to_target_( pool.begin(), pool.end(), tgt, target_pos, thread_id, source );
          }
        }
      } // for target_begin
    }
    catch ( std::exception& err )
    {
      // We must create a new exception here, err's lifetime ends at
      // the end of the catch block.
      exceptions_raised_.at( thread_id ) =
        std::shared_ptr< WrappedThreadException >( new WrappedThreadException( err ) );
    }
  } // omp parallel
  // check if any exceptions have been raised
  for ( thread thr = 0; thr < kernel().vp_manager.get_num_threads(); ++thr )
  {
    if ( exceptions_raised_.at( thr ).get() )
    {
      throw WrappedThreadException( *( exceptions_raised_.at( thr ) ) );
    }
  }
}


template < int D >
void
ConnectionCreator::source_driven_connect_( Layer< D >& source, Layer< D >& target, GIDCollectionPTR target_gc )
{
  // Source driven connect is actually implemented as target driven,
  // but with displacements computed in the target layer. The Mask has been
  // reversed so that it can be applied to the source instead of the target.
  // For each local target node:
  //  1. Apply (Converse)Mask to source layer
  //  2. For each source node: Compute probability, draw random number, make
  //     connection conditionally

  PoolWrapper_< D > pool;
  if ( mask_.get() ) // MaskedLayer will be freed by PoolWrapper d'tor
  {
    // By supplying the target layer to the MaskedLayer constructor, the
    // mask is mirrored so it may be applied to the source layer instead
    pool.define( new MaskedLayer< D >( source, mask_, allow_oversized_, target ) );
  }
  else
  {
    pool.define( source.get_global_positions_vector() );
  }

  std::vector< std::shared_ptr< WrappedThreadException > > exceptions_raised_( kernel().vp_manager.get_num_threads() );

  // We only need to check the first in the GIDCollection
  Node* const first_in_tgt = kernel().node_manager.get_node_or_proxy( target_gc->operator[]( 0 ) );
  if ( not first_in_tgt->has_proxies() )
  {
    throw IllegalConnection(
      "Topology Divergent connections"
      " to devices are not possible." );
  }

// sharing specs on next line commented out because gcc 4.2 cannot handle them
#pragma omp parallel // default(none) shared(source, target, masked_layer,
                     // target_begin, target_end)
  {
    const int thread_id = kernel().vp_manager.get_thread_id();
    try
    {
      GIDCollection::const_iterator target_begin = target_gc->local_begin();
      GIDCollection::const_iterator target_end = target_gc->end();

      for ( GIDCollection::const_iterator tgt_it = target_begin; tgt_it < target_end; ++tgt_it )
      {
        Node* const tgt = kernel().node_manager.get_node_or_proxy( ( *tgt_it ).gid, thread_id );

        assert( not tgt->is_proxy() );

        const Position< D > target_pos = target.get_position( ( *tgt_it ).lid );

        if ( mask_.get() )
        {
          // We do the same as in the target driven case, except that we calculate displacements in the target layer.
          // We therefore send in target as last parameter.
          connect_to_target_( pool.masked_begin( target_pos ), pool.masked_end(), tgt, target_pos, thread_id, target );
        }
        else
        {
          // We do the same as in the target driven case, except that we calculate displacements in the target layer.
          // We therefore send in target as last parameter.
          connect_to_target_( pool.begin(), pool.end(), tgt, target_pos, thread_id, target );
        }

      } // end for
    }
    catch ( std::exception& err )
    {
      // We must create a new exception here, err's lifetime ends at the end of the catch block.
      exceptions_raised_.at( thread_id ) =
        std::shared_ptr< WrappedThreadException >( new WrappedThreadException( err ) );
    }
  } // omp parallel
  // check if any exceptions have been raised
  for ( thread thr = 0; thr < kernel().vp_manager.get_num_threads(); ++thr )
  {
    if ( exceptions_raised_.at( thr ).get() )
    {
      throw WrappedThreadException( *( exceptions_raised_.at( thr ) ) );
    }
  }
}

template < int D >
void
ConnectionCreator::convergent_connect_( Layer< D >& source, Layer< D >& target, GIDCollectionPTR target_gc )
{
  if ( number_of_connections_ < 1 )
  {
    return;
  }

  // Convergent connections (fixed fan in)
  //
  // For each local target node:
  // 1. Apply Mask to source layer
  // 2. Compute connection probability for each source position
  // 3. Draw source nodes and make connections

  // We only need to check the first in the GIDCollection
  Node* const first_in_tgt = kernel().node_manager.get_node_or_proxy( target_gc->operator[]( 0 ) );
  if ( not first_in_tgt->has_proxies() )
  {
    throw IllegalConnection(
      "Topology Convergent connections"
      " to devices are not possible." );
  }

  GIDCollection::const_iterator target_begin = target_gc->MPI_local_begin();
  GIDCollection::const_iterator target_end = target_gc->end();

  // protect against connecting to devices without proxies
  // we need to do this before creating the first connection to leave
  // the network untouched if any target does not have proxies
  for ( GIDCollection::const_iterator tgt_it = target_begin; tgt_it < target_end; ++tgt_it )
  {
    Node* const tgt = kernel().node_manager.get_node_or_proxy( ( *tgt_it ).gid );

    assert( not tgt->is_proxy() );
  }

  if ( mask_.get() )
  {
    MaskedLayer< D > masked_source( source, mask_, allow_oversized_ );
    const auto masked_source_end = masked_source.end();

    std::vector< std::pair< Position< D >, index > > positions;

    for ( GIDCollection::const_iterator tgt_it = target_begin; tgt_it < target_end; ++tgt_it )
    {
      index target_id = ( *tgt_it ).gid;
      Node* const tgt = kernel().node_manager.get_node_or_proxy( target_id );

      thread target_thread = tgt->get_thread();
      librandom::RngPtr rng = get_vp_rng( target_thread );
      Position< D > target_pos = target.get_position( ( *tgt_it ).lid );

      // Get (position,GID) pairs for sources inside mask
      const Position< D > anchor = target.get_position( ( *tgt_it ).lid );

      positions.resize( std::distance( masked_source.begin( anchor ), masked_source_end ) );
      std::copy( masked_source.begin( anchor ), masked_source_end, positions.begin() );

      // We will select `number_of_connections_` sources within the mask.
      // If there is no kernel, we can just draw uniform random numbers,
      // but with a kernel we have to set up a probability distribution
      // function using the Vose class.
      if ( kernel_.get() )
      {

        std::vector< double > probabilities;
        probabilities.reserve( positions.size() );

        // Collect probabilities for the sources
        for ( typename std::vector< std::pair< Position< D >, index > >::iterator iter = positions.begin();
              iter != positions.end();
              ++iter )
        {
          probabilities.push_back( kernel_->value( rng, iter->first.get_vector(), target_pos.get_vector(), source ) );
        }

        if ( positions.empty()
          or ( ( not allow_autapses_ ) and ( positions.size() == 1 ) and ( positions[ 0 ].second == target_id ) )
          or ( ( not allow_multapses_ ) and ( positions.size() < number_of_connections_ ) ) )
        {
          std::string msg = String::compose( "Global target ID %1: Not enough sources found inside mask", target_id );
          throw KernelException( msg.c_str() );
        }

        // A Vose object draws random integers with a non-uniform
        // distribution.
        Vose lottery( probabilities );

        // If multapses are not allowed, we must keep track of which
        // sources have been selected already.
        std::vector< bool > is_selected( positions.size() );

        // Draw `number_of_connections_` sources
        for ( int i = 0; i < ( int ) number_of_connections_; ++i )
        {
          index random_id = lottery.get_random_id( rng );
          if ( ( not allow_multapses_ ) and ( is_selected[ random_id ] ) )
          {
            --i;
            continue;
          }

          index source_id = positions[ random_id ].second;
          if ( ( not allow_autapses_ ) and ( source_id == target_id ) )
          {
            --i;
            continue;
          }
          const double w =
            weight_->value( rng, positions[ random_id ].first.get_vector(), target_pos.get_vector(), source );
          const double d =
            delay_->value( rng, positions[ random_id ].first.get_vector(), target_pos.get_vector(), source );
          kernel().connection_manager.connect( source_id, tgt, target_thread, synapse_model_, dummy_param_, d, w );
          is_selected[ random_id ] = true;
        }
      }
      else
      {

        // no kernel

        if ( positions.empty()
          or ( ( not allow_autapses_ ) and ( positions.size() == 1 ) and ( positions[ 0 ].second == target_id ) )
          or ( ( not allow_multapses_ ) and ( positions.size() < number_of_connections_ ) ) )
        {
          std::string msg = String::compose( "Global target ID %1: Not enough sources found inside mask", target_id );
          throw KernelException( msg.c_str() );
        }

        // If multapses are not allowed, we must keep track of which
        // sources have been selected already.
        std::vector< bool > is_selected( positions.size() );

        // Draw `number_of_connections_` sources
        for ( int i = 0; i < ( int ) number_of_connections_; ++i )
        {
          index random_id = rng->ulrand( positions.size() );
          if ( ( not allow_multapses_ ) and ( is_selected[ random_id ] ) )
          {
            --i;
            continue;
          }
          index source_id = positions[ random_id ].second;
          const double w =
            weight_->value( rng, positions[ random_id ].first.get_vector(), target_pos.get_vector(), source );
          const double d =
            delay_->value( rng, positions[ random_id ].first.get_vector(), target_pos.get_vector(), source );
          kernel().connection_manager.connect( source_id, tgt, target_thread, synapse_model_, dummy_param_, d, w );
          is_selected[ random_id ] = true;
        }
      }
    }
  }
  else
  {
    // no mask

    // Get (position,GID) pairs for all nodes in source layer
    std::vector< std::pair< Position< D >, index > >* positions = source.get_global_positions_vector();

    for ( GIDCollection::const_iterator tgt_it = target_begin; tgt_it < target_end; ++tgt_it )
    {
      index target_id = ( *tgt_it ).gid;
      Node* const tgt = kernel().node_manager.get_node_or_proxy( target_id );
      thread target_thread = tgt->get_thread();
      librandom::RngPtr rng = get_vp_rng( target_thread );
      Position< D > target_pos = target.get_position( ( *tgt_it ).lid );

      if ( ( positions->size() == 0 )
        or ( ( not allow_autapses_ ) and ( positions->size() == 1 ) and ( ( *positions )[ 0 ].second == target_id ) )
        or ( ( not allow_multapses_ ) and ( positions->size() < number_of_connections_ ) ) )
      {
        std::string msg = String::compose( "Global target ID %1: Not enough sources found", target_id );
        throw KernelException( msg.c_str() );
      }

      // We will select `number_of_connections_` sources within the mask.
      // If there is no kernel, we can just draw uniform random numbers,
      // but with a kernel we have to set up a probability distribution
      // function using the Vose class.
      if ( kernel_.get() )
      {

        std::vector< double > probabilities;
        probabilities.reserve( positions->size() );

        // Collect probabilities for the sources
        for ( typename std::vector< std::pair< Position< D >, index > >::iterator iter = positions->begin();
              iter != positions->end();
              ++iter )
        {
          probabilities.push_back( kernel_->value( rng, iter->first.get_vector(), target_pos.get_vector(), source ) );
        }

        // A Vose object draws random integers with a non-uniform
        // distribution.
        Vose lottery( probabilities );

        // If multapses are not allowed, we must keep track of which
        // sources have been selected already.
        std::vector< bool > is_selected( positions->size() );

        // Draw `number_of_connections_` sources
        for ( int i = 0; i < ( int ) number_of_connections_; ++i )
        {
          index random_id = lottery.get_random_id( rng );
          if ( ( not allow_multapses_ ) and ( is_selected[ random_id ] ) )
          {
            --i;
            continue;
          }

          index source_id = ( *positions )[ random_id ].second;
          if ( ( not allow_autapses_ ) and ( source_id == target_id ) )
          {
            --i;
            continue;
          }

          Position< D > source_pos = ( *positions )[ random_id ].first;
          const double w =
            weight_->value( rng, ( *positions )[ random_id ].first.get_vector(), target_pos.get_vector(), source );
          const double d =
            delay_->value( rng, ( *positions )[ random_id ].first.get_vector(), target_pos.get_vector(), source );
          kernel().connection_manager.connect( source_id, tgt, target_thread, synapse_model_, dummy_param_, d, w );
          is_selected[ random_id ] = true;
        }
      }
      else
      {

        // no kernel

        // If multapses are not allowed, we must keep track of which
        // sources have been selected already.
        std::vector< bool > is_selected( positions->size() );

        // Draw `number_of_connections_` sources
        for ( int i = 0; i < ( int ) number_of_connections_; ++i )
        {
          index random_id = rng->ulrand( positions->size() );
          if ( ( not allow_multapses_ ) and ( is_selected[ random_id ] ) )
          {
            --i;
            continue;
          }

          index source_id = ( *positions )[ random_id ].second;
          if ( ( not allow_autapses_ ) and ( source_id == target_id ) )
          {
            --i;
            continue;
          }

          const double w =
            weight_->value( rng, ( *positions )[ random_id ].first.get_vector(), target_pos.get_vector(), source );
          const double d =
            delay_->value( rng, ( *positions )[ random_id ].first.get_vector(), target_pos.get_vector(), source );
          kernel().connection_manager.connect( source_id, tgt, target_thread, synapse_model_, dummy_param_, d, w );
          is_selected[ random_id ] = true;
        }
      }
    }
  }
}


template < int D >
void
ConnectionCreator::divergent_connect_( Layer< D >& source, Layer< D >& target, GIDCollectionPTR target_gc )
{
  if ( number_of_connections_ < 1 )
  {
    return;
  }

  // protect against connecting to devices without proxies
  // we need to do this before creating the first connection to leave
  // the network untouched if any target does not have proxies

  // We only need to check the first in the GIDCollection
  Node* const first_in_tgt = kernel().node_manager.get_node_or_proxy( target_gc->operator[]( 0 ) );
  if ( not first_in_tgt->has_proxies() )
  {
    throw IllegalConnection(
      "Topology Divergent connections"
      " to devices are not possible." );
  }

  GIDCollection::const_iterator target_begin = target_gc->MPI_local_begin();
  GIDCollection::const_iterator target_end = target_gc->end();

  for ( GIDCollection::const_iterator tgt_it = target_begin; tgt_it < target_end; ++tgt_it )
  {
    Node* const tgt = kernel().node_manager.get_node_or_proxy( ( *tgt_it ).gid );

    assert( not tgt->is_proxy() );
  }

  // Divergent connections (fixed fan out)
  //
  // For each (global) source: (All connections made on all mpi procs)
  // 1. Apply mask to global targets
  // 2. If using kernel: Compute connection probability for each global target
  // 3. Draw connections to make using global rng

  MaskedLayer< D > masked_target( target, mask_, allow_oversized_ );
  const auto masked_target_end = masked_target.end();

  std::vector< std::pair< Position< D >, index > >* sources = source.get_global_positions_vector();

  for ( typename std::vector< std::pair< Position< D >, index > >::iterator src_it = sources->begin();
        src_it != sources->end();
        ++src_it )
  {

    Position< D > source_pos = src_it->first;
    index source_id = src_it->second;
    std::vector< double > source_pos_vector = source_pos.get_vector();
    std::vector< index > targets;
    std::vector< std::pair< double, double > > weight_delay_pairs;
    std::vector< double > probabilities;

    // Find potential targets and probabilities

    for ( typename Ntree< D, index >::masked_iterator tgt_it = masked_target.begin( source_pos );
          tgt_it != masked_target_end;
          ++tgt_it )
    {

      if ( ( not allow_autapses_ ) and ( source_id == tgt_it->second ) )
      {
        continue;
      }

      librandom::RngPtr rng = get_global_rng();

      targets.push_back( tgt_it->second );
      weight_delay_pairs.emplace_back( weight_->value( rng, source_pos_vector, tgt_it->first.get_vector(), target ),
        delay_->value( rng, source_pos_vector, tgt_it->first.get_vector(), target ) );

      if ( kernel_.get() )
      {
        // TODO: Why do we switch source and target for this displacement?
        probabilities.push_back( kernel_->value( rng, source_pos_vector, tgt_it->first.get_vector(), source ) );
      }
      else
      {
        probabilities.push_back( 1.0 );
      }
    }

    if ( targets.empty() or ( ( not allow_multapses_ ) and ( targets.size() < number_of_connections_ ) ) )
    {
      std::string msg = String::compose( "Global source ID %1: Not enough targets found", source_id );
      throw KernelException( msg.c_str() );
    }

    // Draw targets.  A Vose object draws random integers with a
    // non-uniform distribution.
    Vose lottery( probabilities );

    // If multapses are not allowed, we must keep track of which
    // targets have been selected already.
    std::vector< bool > is_selected( targets.size() );

    // Draw `number_of_connections_` targets
    for ( long i = 0; i < ( long ) number_of_connections_; ++i )
    {
      index random_id = lottery.get_random_id( get_global_rng() );
      if ( ( not allow_multapses_ ) and ( is_selected[ random_id ] ) )
      {
        --i;
        continue;
      }
      is_selected[ random_id ] = true;
      auto target_weight_delay = weight_delay_pairs[ random_id ];
      index target_id = targets[ random_id ];

      // We bail out for non-local neurons only now after all possible
      // random numbers haven been drawn. Bailing out any earlier may lead
      // to desynchronized global rngs.
      if ( not kernel().node_manager.is_local_gid( target_id ) )
      {
        continue;
      }

      Node* target_ptr = kernel().node_manager.get_node_or_proxy( target_id );
      kernel().connection_manager.connect( source_id,
        target_ptr,
        target_ptr->get_thread(),
        synapse_model_,
        dummy_param_,
        target_weight_delay.second,
        target_weight_delay.first );
    }
  }
}

} // namespace nest

#endif
