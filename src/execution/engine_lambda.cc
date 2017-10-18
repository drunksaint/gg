/* -*-mode:c++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */

#include "engine_lambda.hh"

#include <stdexcept>

#include "ggpaths.hh"
#include "optional.hh"
#include "system_runner.hh"
#include "http_response.hh"
#include "remote_response.hh"

using namespace std;
using namespace gg::thunk;

HTTPRequest AWSLambdaExecutionEngine::generate_request( const Thunk & thunk,
                                                        const string & thunk_hash,
                                                        const bool timelog )
{
  const string function_name = "gg-" + thunk.executable_hash();

  return LambdaInvocationRequest(
    credentials_, region_, function_name, thunk.execution_payload( thunk_hash, timelog ),
    LambdaInvocationRequest::InvocationType::REQUEST_RESPONSE,
    LambdaInvocationRequest::LogType::NONE
  ).to_http_request();
}

void AWSLambdaExecutionEngine::force_thunk( const string & hash,
                                            const Thunk & thunk )
{
  HTTPRequest request = generate_request( thunk, hash, false );

  TCPSocket sock;
  sock.set_blocking( false );
  try {
    sock.connect( address_ );
    throw runtime_error( "nonblocking connect unexpectedly succeeded immediately" );
  } catch ( const unix_error & e ) {
    if ( e.error_code() == EINPROGRESS ) {
      /* do nothing */
    } else {
      throw;
    }
  }

  SecureSocket lambda_socket = ssl_context_.new_secure_socket( move( sock ) );

  exec_loop_.add_connection(
    hash,
    [this] ( const string & thunk_hash, const HTTPResponse & http_response )
    {
      if ( http_response.status_code() != "200" ) {
        throw runtime_error( "HTTP failure: " + http_response.status_code() );
      }

      RemoteResponse response = RemoteResponse::parse_message( http_response.body() );

      if ( response.type != RemoteResponse::Type::SUCCESS ) {
        throw runtime_error( "execution failed." );
      }

      if ( response.thunk_hash != thunk_hash ) {
        cerr << http_response.str() << endl;
        throw runtime_error( "expected output for " + thunk_hash + ", got output for " + response.thunk_hash );
      }

      gg::cache::insert( response.thunk_hash, response.output_hash );
      callback_( response.thunk_hash, response.output_hash );
    },
    lambda_socket, request
  );

  running_jobs_++;
}

size_t AWSLambdaExecutionEngine::job_count() const
{
  return running_jobs_;
}
