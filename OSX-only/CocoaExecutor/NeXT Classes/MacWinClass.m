
/* Generated by Interface Builder */

#import "MacWinClass.h"

@implementation MacWindow

- (BOOL)commandKey:(NSEvent *)theEvent
{
    if ( [[self contentView] performKeyEquivalent:theEvent] )
        return( YES );
    else
        return( NO );
}

extern void ROMlib_dummywincall( void );
void ROMlib_dummywincall( void )
{
}

@end