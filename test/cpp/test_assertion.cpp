// The client assertion a certificate credential sends instead of a client
// secret (#71): a JWT signed RS256 with the certificate's key, its SHA-1
// thumbprint as `x5t`. Pure; the signing itself is test_certificate.cpp.
#include "sharepoint_assertion.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace duckdb::sharepoint;

TEST_CASE("Base64Url: no padding, URL alphabet", "[assertion]") {
	REQUIRE(Base64Url("") == "");
	REQUIRE(Base64Url("f") == "Zg");
	REQUIRE(Base64Url("fo") == "Zm8");
	REQUIRE(Base64Url("foo") == "Zm9v");
	REQUIRE(Base64Url(std::string("\xfb\xff\xbf", 3)) == "-_-_");
}

TEST_CASE("ClientAssertionSigningInput: header and claims Entra expects", "[assertion]") {
	const auto input =
	    ClientAssertionSigningInput("app-id", "https://login.microsoftonline.com/contoso.onmicrosoft.com/oauth2/v2.0/token",
	                                "THUMB", "jti-1", 1700000000);
	const auto dot = input.find('.');
	REQUIRE(dot != std::string::npos);
	REQUIRE(input.find('.', dot + 1) == std::string::npos); // header.payload, not yet signed
	REQUIRE(input.substr(0, dot) == Base64Url(R"({"alg":"RS256","typ":"JWT","x5t":"THUMB"})"));
	REQUIRE(input.substr(dot + 1) ==
	        Base64Url(R"({"aud":"https://login.microsoftonline.com/contoso.onmicrosoft.com/oauth2/v2.0/token",)"
	                  R"("iss":"app-id","sub":"app-id","jti":"jti-1","nbf":1700000000,"iat":1700000000,)"
	                  R"("exp":1700000600})"));
}

TEST_CASE("ClientAssertionSigningInput escapes JSON", "[assertion]") {
	const auto input = ClientAssertionSigningInput("a\"b\\c", "aud", "x", "j", 0);
	REQUIRE(input.substr(input.find('.') + 1) ==
	        Base64Url(R"({"aud":"aud","iss":"a\"b\\c","sub":"a\"b\\c","jti":"j","nbf":0,"iat":0,"exp":600})"));
}
