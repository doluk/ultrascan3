// test_us_master_password.cpp - GUI-free master-password logic
// The master-password unit is not built in NO_DB (HPC) configurations.
#ifndef NO_DB
#include "qt_test_base.h"
#include "us_master_password.h"
#include "us_os_auth.h"
#include "us_settings.h"

#include <QCryptographicHash>

#include <cstdio>
#include <cstring>

class TestUSMasterPassword : public QtTestBase
{
  protected:
    void SetUp() override
    {
        QtTestBase::SetUp();
        originalHash     = US_Settings::UltraScanPW();
        originalKeychain = US_Settings::masterPwInKeychain();
        originalOsAudit  = US_Settings::auditUsesOsAuth();
        originalCache    = US_MasterPassword::cached();
        hadEnv           = qEnvironmentVariableIsSet( US_MasterPassword::envVarName() );
        originalEnv      = qgetenv( US_MasterPassword::envVarName() );

        // Never touch the developer's real keychain from unit tests.
        US_Settings::set_masterPwInKeychain( false );
        US_Settings::set_UltraScanPW( US_MasterPassword::hash( secret ) );
        US_MasterPassword::clearCache();
        qunsetenv( US_MasterPassword::envVarName() );
    }

    void TearDown() override
    {
        US_Settings::set_UltraScanPW( originalHash );
        US_Settings::set_masterPwInKeychain( originalKeychain );
        US_Settings::set_auditUsesOsAuth( originalOsAudit );
        US_MasterPassword::clearCache();
        if ( ! originalCache.isEmpty() && US_MasterPassword::verify( originalCache ) )
            US_MasterPassword::accept( originalCache );
        if ( hadEnv )
            qputenv( US_MasterPassword::envVarName(), originalEnv );
        else
            qunsetenv( US_MasterPassword::envVarName() );
        QtTestBase::TearDown();
    }

    // A stream holding \p text, positioned at its start.
    static FILE* streamWith( const char* text )
    {
        FILE* f = std::tmpfile();
        if ( f == nullptr ) return nullptr;
        std::fputs( text, f );
        std::rewind( f );
        return f;
    }

    const QString secret = QStringLiteral( "S3cret-master" );

    QByteArray originalHash;
    bool       originalKeychain = false;
    bool       originalOsAudit  = false;
    QString    originalCache;
    bool       hadEnv = false;
    QByteArray originalEnv;
};

TEST_F( TestUSMasterPassword, HashMatchesStoredFormat )
{
    EXPECT_EQ( US_MasterPassword::hash( secret ),
               QCryptographicHash::hash( secret.toLatin1(), QCryptographicHash::Sha1 ) );
}

TEST_F( TestUSMasterPassword, VerifyAcceptsOnlyTheMasterPassword )
{
    EXPECT_TRUE ( US_MasterPassword::isSet() );
    EXPECT_TRUE ( US_MasterPassword::verify( secret ) );
    EXPECT_FALSE( US_MasterPassword::verify( "wrong" ) );
    EXPECT_FALSE( US_MasterPassword::verify( QString() ) );
}

TEST_F( TestUSMasterPassword, VerifyFailsWhenNoMasterPasswordIsSet )
{
    US_Settings::set_UltraScanPW( QByteArray() );
    EXPECT_FALSE( US_MasterPassword::isSet() );
    EXPECT_FALSE( US_MasterPassword::verify( secret ) );
    EXPECT_FALSE( US_MasterPassword::accept( secret ) );
}

TEST_F( TestUSMasterPassword, AcceptCachesOnlyCorrectPasswords )
{
    EXPECT_TRUE( US_MasterPassword::cached().isEmpty() );

    EXPECT_FALSE( US_MasterPassword::accept( "wrong" ) );
    EXPECT_TRUE ( US_MasterPassword::cached().isEmpty() );

    EXPECT_TRUE ( US_MasterPassword::accept( secret ) );
    EXPECT_EQ   ( US_MasterPassword::cached(), secret );

    US_MasterPassword::clearCache();
    EXPECT_TRUE ( US_MasterPassword::cached().isEmpty() );
}

TEST_F( TestUSMasterPassword, NonInteractiveReturnsCachedPassword )
{
    QString err;
    EXPECT_TRUE( US_MasterPassword::obtainNonInteractive( &err ).isEmpty() );
    EXPECT_FALSE( err.isEmpty() );

    ASSERT_TRUE( US_MasterPassword::accept( secret ) );
    EXPECT_EQ( US_MasterPassword::obtainNonInteractive(), secret );
}

TEST_F( TestUSMasterPassword, EnvironmentVariableIsCheckedAgainstHash )
{
    QString err;
    EXPECT_TRUE ( US_MasterPassword::fromEnvironment( &err ).isEmpty() );
    EXPECT_TRUE ( err.contains( US_MasterPassword::envVarName() ) );

    qputenv( US_MasterPassword::envVarName(), "wrong" );
    EXPECT_TRUE ( US_MasterPassword::fromEnvironment().isEmpty() );
    EXPECT_TRUE ( US_MasterPassword::cached().isEmpty() );

    qputenv( US_MasterPassword::envVarName(), secret.toLatin1() );
    EXPECT_EQ   ( US_MasterPassword::fromEnvironment(), secret );
    EXPECT_EQ   ( US_MasterPassword::cached(), secret );
}

TEST_F( TestUSMasterPassword, HeadlessUsesEnvironmentWhenCacheIsEmpty )
{
    qputenv( US_MasterPassword::envVarName(), secret.toLatin1() );
    EXPECT_EQ( US_MasterPassword::obtainHeadless(), secret );
}

TEST_F( TestUSMasterPassword, PromptLoopRetriesUntilCorrect )
{
    FILE* in  = streamWith( "wrong\r\n" "S3cret-master\n" );
    FILE* out = std::tmpfile();
    ASSERT_NE( in, nullptr );
    ASSERT_NE( out, nullptr );

    EXPECT_EQ( US_MasterPassword::promptLoop( in, out, 3 ), secret );
    EXPECT_EQ( US_MasterPassword::cached(), secret );

    // The password itself is never written to the output stream.
    std::rewind( out );
    char buf[ 512 ] = { 0 };
    const size_t n = std::fread( buf, 1, sizeof( buf ) - 1, out );
    EXPECT_GT( n, 0u );
    EXPECT_EQ( std::strstr( buf, "S3cret" ), nullptr );
    EXPECT_NE( std::strstr( buf, "Incorrect password" ), nullptr );

    std::fclose( in );
    std::fclose( out );
}

TEST_F( TestUSMasterPassword, PromptLoopGivesUpAfterAttempts )
{
    FILE* in  = streamWith( "a\nb\nS3cret-master\n" );
    FILE* out = std::tmpfile();
    ASSERT_NE( in, nullptr );
    ASSERT_NE( out, nullptr );

    QString err;
    EXPECT_TRUE( US_MasterPassword::promptLoop( in, out, 2, &err ).isEmpty() );
    EXPECT_FALSE( err.isEmpty() );
    EXPECT_TRUE( US_MasterPassword::cached().isEmpty() );

    std::fclose( in );
    std::fclose( out );
}

TEST_F( TestUSMasterPassword, PromptLoopStopsAtEndOfInput )
{
    FILE* in  = streamWith( "" );
    FILE* out = std::tmpfile();
    ASSERT_NE( in, nullptr );
    ASSERT_NE( out, nullptr );

    QString err;
    EXPECT_TRUE( US_MasterPassword::promptLoop( in, out, 3, &err ).isEmpty() );
    EXPECT_FALSE( err.isEmpty() );

    std::fclose( in );
    std::fclose( out );
}

TEST_F( TestUSMasterPassword, SecurityOptionsDefaultOffAndRoundTrip )
{
    US_Settings::set_masterPwInKeychain( false );
    US_Settings::set_auditUsesOsAuth( false );
    EXPECT_FALSE( US_Settings::masterPwInKeychain() );
    EXPECT_FALSE( US_Settings::auditUsesOsAuth() );

    US_Settings::set_auditUsesOsAuth( true );
    EXPECT_TRUE ( US_Settings::auditUsesOsAuth() );
    US_Settings::set_auditUsesOsAuth( false );
    EXPECT_FALSE( US_Settings::auditUsesOsAuth() );
}

TEST_F( TestUSMasterPassword, OsAuthRejectsEmptyPassword )
{
    // Only the empty password is tested: a wrong password would count as a
    // failed login of the developer's account (e.g. pam_faillock).
    QString err;
    EXPECT_FALSE( US_OsAuth::verifyCurrentUser( QString(), &err ) );
    EXPECT_FALSE( err.isEmpty() );
    EXPECT_FALSE( US_OsAuth::currentUser().isEmpty() );
}

#endif // NO_DB
