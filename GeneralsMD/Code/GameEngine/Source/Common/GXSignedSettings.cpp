/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// GeneralsX @feature Android port 18/10/2026 See GXSignedSettings.h.

#include "PreRTS.h"

#include "Common/GXSignedSettings.h"
#include "Common/GXRemoteConfig.h"

#include <cstdio>
#include <map>
#include <string>
#include <vector>

#if defined(__ANDROID__)
#include <openssl/evp.h>
#include <openssl/x509.h>
#include "GameNetwork/GeneralsOnline/json.hpp"
#endif

namespace
{
#if defined(__ANDROID__)
	// SubjectPublicKeyInfo of the update signing key: the same bytes as the launcher's
	// UpdateManager.PUBLIC_KEY_B64.
	const char kPublicKeyB64[] =
		"MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEFjy+4K0lTRmnwQe+nQqXreCMtJehCl1wiYNgq5Rr/MHWkDukps0eUbmuyxSenyFL4T5zo+WBIFeLDO5PXFoG/A==";

	bool readFile(const std::string &path, std::string &out, size_t limit)
	{
		FILE *f = fopen(path.c_str(), "rb");
		if (f == nullptr)
			return false;
		char buf[4096];
		size_t n;
		out.clear();
		while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
		{
			out.append(buf, n);
			if (out.size() > limit)
			{
				fclose(f);
				return false;
			}
		}
		fclose(f);
		return true;
	}

	bool decodeBase64(std::string text, std::vector<unsigned char> &out)
	{
		std::string clean;
		for (char c : text)
		{
			if (c != '\n' && c != '\r' && c != ' ' && c != '\t')
				clean += c;
		}
		if (clean.empty() || clean.size() % 4 != 0)
			return false;
		out.resize(clean.size() / 4 * 3);
		const int n = EVP_DecodeBlock(out.data(), reinterpret_cast<const unsigned char *>(clean.data()), (int)clean.size());
		if (n < 0)
			return false;
		size_t len = (size_t)n;
		if (clean[clean.size() - 1] == '=')
			--len;
		if (clean[clean.size() - 2] == '=')
			--len;
		out.resize(len);
		return true;
	}

	bool verify(const std::string &message, const std::vector<unsigned char> &signature)
	{
		std::vector<unsigned char> keyDer;
		if (!decodeBase64(kPublicKeyB64, keyDer))
			return false;
		const unsigned char *p = keyDer.data();
		EVP_PKEY *key = d2i_PUBKEY(nullptr, &p, (long)keyDer.size());
		if (key == nullptr)
			return false;
		EVP_MD_CTX *ctx = EVP_MD_CTX_new();
		bool ok = ctx != nullptr
			&& EVP_DigestVerifyInit(ctx, nullptr, EVP_sha256(), nullptr, key) == 1
			&& EVP_DigestVerify(ctx, signature.data(), signature.size(),
				reinterpret_cast<const unsigned char *>(message.data()), message.size()) == 1;
		EVP_MD_CTX_free(ctx);
		EVP_PKEY_free(key);
		return ok;
	}

	const std::map<std::string, std::string> &signedConfig()
	{
		static std::map<std::string, std::string> s_values;
		static bool s_loaded = false;
		if (s_loaded)
			return s_values;
		s_loaded = true;

		const std::string dir = GXRemoteConfig::updateDirPath();
		std::string manifest, signatureText;
		std::vector<unsigned char> signature;
		if (dir.empty()
			|| !readFile(dir + "/manifest.json", manifest, 256 * 1024)
			|| !readFile(dir + "/manifest.json.sig", signatureText, 16 * 1024)
			|| !decodeBase64(signatureText, signature)
			|| !verify(manifest, signature))
		{
			fprintf(stderr, "[GX-Settings] no verified signed manifest: signed feature switches are off\n");
			fflush(stderr);
			return s_values;
		}
		try
		{
			const nlohmann::json doc = nlohmann::json::parse(manifest);
			if (doc.contains("config") && doc["config"].is_object())
			{
				for (auto it = doc["config"].begin(); it != doc["config"].end(); ++it)
				{
					if (it.value().is_string())
						s_values[it.key()] = it.value().get<std::string>();
				}
			}
		}
		catch (const std::exception &e)
		{
			fprintf(stderr, "[GX-Settings] signed manifest unreadable (%s): signed feature switches are off\n", e.what());
			fflush(stderr);
			s_values.clear();
			return s_values;
		}
		fprintf(stderr, "[GX-Settings] signed manifest verified (%u settings)\n", (unsigned)s_values.size());
		fflush(stderr);
		return s_values;
	}
#endif
}

bool GXSignedSettings::enabled(const char *feature)
{
#if defined(__ANDROID__)
	const std::map<std::string, std::string> &v = signedConfig();
	std::map<std::string, std::string>::const_iterator it = v.find(feature);
	return it != v.end() && it->second == "1";
#else
	(void)feature;
	return false;
#endif
}
