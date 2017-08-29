/* -*-mode:c++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */

#include <vector>
#include <string>
#include <tuple>
#include <algorithm>
#include <map>
#include <array>
#include <memory>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <getopt.h>
#include <libgen.h>

#include "exception.hh"
#include "optional.hh"
#include "digest.hh"
#include "temp_file.hh"
#include "thunk.hh"
#include "ggpaths.hh"
#include "system_runner.hh"

#include "model-gcc.hh"

using namespace std;
using namespace gg::thunk;

void dump_gcc_specs( TempFile & target_file )
{
  target_file.write( check_output( "gcc-7 -dumpspecs" ) );
}

vector<string> prune_makedep_flags( const vector<string> & args )
{
  vector<string> result;

  for ( auto it = args.begin(); it != args.end(); it++ ) {
    if ( ( *it == "-M" ) or ( *it == "-MD" ) or ( *it == "-MP" ) ) {
      continue;
    }
    else if ( ( *it == "-MF" ) or ( *it == "-MT" ) ) {
      it++;
    }
    else {
      result.push_back( *it );
    }
  }

  return result;
}

bool is_non_object_input( const InputFile & input )
{
  switch( input.language ) {
  case Language::SHARED_LIBRARY:
  case Language::ARCHIVE_LIBRARY:
  case Language::OBJECT:
    return false;

  default: return true;
  }
}

Thunk GCCModelGenerator::generate_thunk( const GCCStage stage,
                                         const InputFile & input,
                                         const string & output )
{
  vector<string> args { arguments_.option_args() };

  args.erase(
    remove_if(
      args.begin(), args.end(),
      []( const string & s ) { return ( s == "-E" or s == "-S" or s == "-c" ); }
    ), args.end()
  );

  /* Common infiles */
  vector<InFile> base_infiles = {
    input.infile,
    program_infiles.at( ( operation_mode_ == OperationMode::GCC ) ? GCC : GXX )
  };

  base_infiles.emplace_back( "/__gg__/gcc-specs", specs_tempfile_.name() );

  /* Common args */
  args.push_back( "-x" );
  args.push_back( language_to_name( input.language ) );
  args.push_back( input.name );

  if ( stage != PREPROCESS ) {
    /* For preprocess stage, we are going to use `args` to get the
       dependencies, so we can't have this FAKE specs path here.
       We will add these later.*/
    args.insert( args.begin(), "-specs=/__gg__/gcc-specs" );
    args.push_back( "-o" );
    args.push_back( output );
  }

  bool generate_makedep_file = find_if( args.begin(), args.end(),
      [] ( const string & dep ) {
        return ( dep == "-M" ) or ( dep == "-MM" ) or ( dep == "-MD" );
      }
    ) != end( args );

  switch ( stage ) {
  case PREPROCESS:
  {
    const vector<string> & include_path =
      ( input.language == Language::C or input.language == Language::C_HEADER )
        ? c_include_path
        : cpp_include_path;

     /* ARGS */
    args.push_back( "-E" );

    args.push_back( "-frandom-seed=" + BEGIN_REPLACE
                    + input.name
                    + END_REPLACE );

    args.push_back( "-Werror=date-time" );

    vector<string> all_args;
    all_args.reserve( c_include_path.size() + args.size() + 2 );
    all_args.push_back( "-nostdinc" );

    if ( input.language == Language::CXX or
         input.language == Language::CXX_HEADER ) {
      all_args.push_back( "-nostdinc++" );
    }

    for ( const auto & p : include_path ) {
      all_args.push_back( "-isystem" + p );
    }

    all_args.insert( all_args.end(), args.begin(), args.end() );

    /* Generate dependency list */
    TempFile makedep_tempfile { "/tmp/gg-makedep" };
    string makedep_filename;
    string makedep_target = input.name;

    if ( generate_makedep_file ) {
      cerr << "[+] generating make dependencies file..." << endl;
      generate_dependencies_file( all_args, input.name, "" );
      makedep_filename = *arguments_.option_argument( GCCOption::MF );
      Optional<string> mt_arg = arguments_.option_argument( GCCOption::MT );
      if ( mt_arg.initialized() ) {
        makedep_target = *mt_arg;
      }
    }
    else {
      generate_dependencies_file( args, input.name, makedep_tempfile.name() );
      makedep_filename = makedep_tempfile.name();
    }

    vector<string> dependencies = parse_dependencies_file( makedep_filename, makedep_target );

    all_args.insert( all_args.begin(), "-specs=/__gg__/gcc-specs" );
    all_args.push_back( "-o" );
    all_args.push_back( output );
    all_args = prune_makedep_flags( all_args );

    /* INFILES */
    if ( input.language == Language::C or input.language == Language::C_HEADER ) {
      base_infiles.emplace_back( program_infiles.at( CC1 ) );
    }
    else {
      base_infiles.emplace_back( program_infiles.at( CC1PLUS ) );
    }

    for ( const string & dep : dependencies ) {
      base_infiles.emplace_back( dep );
    }

    for ( const string & dir : include_path ) {
      base_infiles.emplace_back( dir, "", InFile::Type::DUMMY_DIRECTORY );
    }

    for ( const string & dir : arguments_.include_dirs() ) {
      base_infiles.emplace_back( dir, "", InFile::Type::DUMMY_DIRECTORY );
    }

    base_infiles.emplace_back( ".", "", InFile::Type::DUMMY_DIRECTORY );

    return { output, gcc_function( operation_mode_, all_args, envars_ ), base_infiles };
  }

  case COMPILE:
    args.push_back( "-S" );

    args.push_back( "-frandom-seed=__GG_BEGIN_REPLACE__"
                    + input.name
                    + "__GG_END_REPLACE__" );

    args = prune_makedep_flags( args );

    if ( input.language == Language::CPP_OUTPUT ) {
      base_infiles.push_back( program_infiles.at( CC1 ) );
    }
    else {
      base_infiles.push_back( program_infiles.at( CC1PLUS ) );
    }

    return { output, gcc_function( operation_mode_, args, envars_ ), base_infiles };

  case ASSEMBLE:
    args.push_back( "-c" );
    args = prune_makedep_flags( args );
    base_infiles.push_back( program_infiles.at( AS ) );
    return { output, gcc_function( operation_mode_, args, envars_ ), base_infiles };

  default: throw runtime_error( "not implemented" );
  }
}

GCCModelGenerator::GCCModelGenerator( const OperationMode operation_mode,
                                      int argc, char ** argv )
  : operation_mode_( operation_mode ), arguments_( argc, argv )
{
  if ( arguments_.input_files().size() == 0 ) {
    throw runtime_error( "no input files" );
  }

  dump_gcc_specs( specs_tempfile_ );

  size_t non_object_inputs = std::count_if( arguments_.input_files().begin(),
                                            arguments_.input_files().end(),
                                            is_non_object_input );

  if ( non_object_inputs > 0 && arguments_.input_files().size() > 1 ) {
    throw runtime_error( "multiple inputs are only allowed for linking" );
  }
}

void GCCModelGenerator::generate()
{
  string final_output = arguments_.output_filename();
  GCCStage last_stage = arguments_.last_stage();
  vector<string> args = arguments_.all_args();

  vector<InputFile> input_files = arguments_.input_files();

  for ( InputFile & input : input_files ) {
    if ( not is_non_object_input( input ) ) {
      continue;
    }

    GCCStage first_stage = language_to_stage( input.language );
    GCCStage input_last_stage = ( last_stage == LINK ) ? ASSEMBLE : last_stage;

    map<size_t, string> stage_output;
    stage_output[ first_stage - 1 ] = input.name;
    input.infile = input.name;

    for ( size_t stage_num = first_stage; stage_num <= input_last_stage; stage_num++ ) {
      GCCStage stage = static_cast<GCCStage>( stage_num );
      string output_name;

      if ( stage == last_stage ) {
        output_name = final_output;
      }
      else {
        output_name = "output_" + to_string( stage_num );
      }

      Thunk stage_thunk = generate_thunk( stage, input, output_name );

      const string last_stage_hash = stage_thunk.store( stage == last_stage );

      switch ( stage ) {
      case PREPROCESS:
        /* generate preprocess thunk */
        cerr << ">> preprocessing " << stage_output[ stage - 1 ] << endl;

        switch ( input.language ) {
        case Language::C:
        case Language::C_HEADER:
          input.language = Language::CPP_OUTPUT;
          break;

        case Language::CXX_HEADER:
        case Language::CXX:
          input.language = Language::CXX_CPP_OUTPUT;
          break;

        default:
          throw runtime_error( "invalid preprocessing language" );
        }

        break;

      case COMPILE:
        /* generate compile thunk */
        cerr << ">> compiling " << stage_output[ stage - 1 ] << endl;
        input.language = Language::ASSEMBLER;
        break;

      case ASSEMBLE:
      {
        /* generate assemble thunk */
        cerr << ">> assembling " << stage_output[ stage - 1 ] << endl;
        input.language = Language::OBJECT;
        break;
      }

      default:
        throw runtime_error( "unexcepted stage" );
      }

      input.name = output_name;
      input.infile = InFile( input.name, "nonexistent",
                             last_stage_hash, stage_thunk.order(),
                             0 );
      cerr << "[output=" << output_name << "]" << endl;
    }
  }

  if ( last_stage == LINK ) {
    if ( final_output.length() == 0 ) {
      final_output = "a.out";
    }

    vector<string> link_args { args };

    cerr << ">> linking ";
    for ( auto const & link_input : input_files ) {
      cerr << link_input.name << " ";
    }
    cerr << endl;

    vector<string> dependencies = get_link_dependencies( input_files, args );

    Thunk thunk = generate_link_thunk( input_files, dependencies, final_output );
    thunk.store();
  }
}

void usage( const char * arg0 )
{
  cerr << arg0 << " (gcc|g++) [GCC ARGUMENTS]" << endl;
}

int main( int argc, char * argv[] )
{
  try {
    if ( argc <= 0 ) {
      abort();
    }

    if ( argc < 3 ) {
      usage( argv[ 0 ] );
      return EXIT_FAILURE;
    }

    OperationMode operation_mode;

    if ( strcmp( argv[ 1 ], "gcc" ) == 0 ) {
      operation_mode = OperationMode::GCC;
    }
    else if ( strcmp( argv[ 1 ], "g++" ) == 0 ) {
      operation_mode = OperationMode::GXX;
    }
    else {
      throw runtime_error( "invalid operation mode" );
    }

    argv++;
    argc--;

    GCCModelGenerator gcc_model_generator { operation_mode, argc, argv };
    gcc_model_generator.generate();

    return EXIT_SUCCESS;
  }
  catch ( const exception & e ) {
    print_exception( argv[ 0 ], e );
    return EXIT_FAILURE;
  }
}
