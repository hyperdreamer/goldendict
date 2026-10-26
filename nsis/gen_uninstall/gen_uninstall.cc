// This program generates uninstallation script automatically, using the
// output of the makensis command.
// Why is this not part of NSIS? Ask NSIS developers.

#include <QtCore>

int main( int argc, char *argv[] )
{
  QCoreApplication app( argc, argv );

  QTextStream stream( stdin );

  auto const exact = []( QString const & pattern ) {
    return QRegularExpression( QRegularExpression::anchoredPattern( pattern ),
                               QRegularExpression::DotMatchesEverythingOption );
  };
  QRegularExpression const setOutPath = exact( "^SetOutPath: \"(.*)\"$" );
  QRegularExpression const createDirectory = exact( "^CreateDirectory: \"(.*)\"$" );
  QRegularExpression const file = exact( "^File: \"([^\"]*)\"(->\"([^\"]*)\")?.*$" );
  QRegularExpression const createShortCut = exact( "^CreateShortCut: \"([^\"]*)\"(->\"([^\"]*)\")?.*$" );
  for( QRegularExpression const * expression : { &setOutPath, &createDirectory, &file, &createShortCut } )
    if( !expression->isValid() )
    {
      qCritical( "Invalid uninstall pattern: %s", qPrintable( expression->errorString() ) );
      return 1;
    }

  QStringList log;

  QString currentOutPath;

  for( QString line; !( line = stream.readLine() ).isNull(); )
  {
    QRegularExpressionMatch match;
    if ( ( match = setOutPath.match( line ) ).hasMatch() )
    {
      qDebug( "Setting out path to %s", qPrintable( match.captured( 1 ) ) );

      currentOutPath = match.captured( 1 );
    }
    else
    if ( ( match = createDirectory.match( line ) ).hasMatch() )
    {
      qDebug( "Creating directory %s", qPrintable( match.captured( 1 ) ) );

      log.append( "RMDir \"" + match.captured( 1 ) + "\"" );
    }
    else
    if ( ( match = file.match( line ) ).hasMatch() )
    {
      QString command( "Delete \"" );

      if ( match.captured( 3 ).isEmpty() )
      {
        // Using the current out path
        command += currentOutPath + "\\" + match.captured( 1 );
      }
      else
      {
        // Use the complete path available
        command += match.captured( 3 );
      }

      command += "\"";

      log.append( command );

      qDebug( "Writing file %s (%s)", qPrintable( match.captured( 1 ) ),
             qPrintable( match.captured( 3 ) ) );
    }
    else
    if ( ( match = createShortCut.match( line ) ).hasMatch() )
    {
      qDebug( "Creating shortcut %s", qPrintable( match.captured( 1 ) ) );

      log.append( "Delete \"" + match.captured( 1 ) + "\"" );
    }
  }

  // Ok, replay the log back

  for( int x = log.size(); x--; )
  {
    printf( "%s\n", qPrintable( log[ x ] ) );
  }

  return 0;
}
