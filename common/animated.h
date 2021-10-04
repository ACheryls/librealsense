// License: Apache 2.0. See LICENSE file in root directory.
// Copyright(c) 2021 Intel Corporation. All Rights Reserved.

#pragma once

#include <chrono>


namespace rs2 {


// Helper class that lets smoothly animate between its values
template < class T > class animated
{
private:
    T _old, _new;
    std::chrono::system_clock::time_point _last_update;
    std::chrono::system_clock::duration _duration;

public:
    animated( T def, std::chrono::system_clock::duration duration = std::chrono::milliseconds( 200 ) )
        : _duration( duration )
        , _old( def )
        , _new( def )
    {
        static_assert( ( std::is_arithmetic< T >::value ), "animated class supports arithmetic built-in types only" );
        _last_update = std::chrono::system_clock::now();
    }
    animated & operator=( const T & other )
    {
        if( other != _new )
        {
            _old = get();
            _new = other;
            _last_update = std::chrono::system_clock::now();
        }
        return *this;
    }
    T get() const
    {
        auto now = std::chrono::system_clock::now();
        auto ms = std::chrono::duration_cast< std::chrono::microseconds >( now - _last_update ).count();
        auto duration_ms = std::chrono::duration_cast< std::chrono::microseconds >( _duration ).count();
        auto t = (float)ms / duration_ms;
        t = std::max( 0.f, std::min( rs2::smoothstep( t, 0.f, 1.f ), 1.f ) );
        return static_cast< T >( _old * ( 1.f - t ) + _new * t );
    }
    operator T() const { return get(); }
    T value() const { return _new; }
};


}  // namespace rs2
