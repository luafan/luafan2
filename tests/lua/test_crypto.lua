--[[
  test_crypto.lua — M12.1 + M12.3 fan.crypto contract tests.

  Covers:
    fan.crypto.md5(data)          -> 32-char lowercase hex
    fan.crypto.md5_binary(data)   -> raw 16-byte digest
    fan.crypto.gcm.encrypt/decrypt with AES-128-GCM and AES-256-GCM,
    optional AAD, tag tamper rejection, invalid-argument surface.

  Runs entirely offline; no fan loop, no sockets. Skips politely if the
  build was compiled without FAN_WITH_OPENSSL — but our CI always enables it.
]]
local T   = require("test_framework")
local fan = require("fan")

local crypto = fan.crypto
if type(crypto) ~= "table" or not crypto.available() then
  print("[SKIP] fan.crypto not available (build without OpenSSL)")
  os.exit(0)
end

local s = T.suite("fan.crypto (M12.1 + M12.3)")

-- ---- md5 -----------------------------------------------------------------

-- Known-answer tests from RFC 1321 Appendix A.5 ("MD5 test suite").
local MD5_KAT = {
  {"",                                                       "d41d8cd98f00b204e9800998ecf8427e"},
  {"a",                                                      "0cc175b9c0f1b6a831c399e269772661"},
  {"abc",                                                    "900150983cd24fb0d6963f7d28e17f72"},
  {"message digest",                                         "f96b697d7cb7938d525a2f31aaf161d0"},
  {"abcdefghijklmnopqrstuvwxyz",                             "c3fcd3d76192e4007dfb496cca67e13b"},
  {"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789",
                                                             "d174ab98d277d9f5a5611c2c9f419d9f"},
  {"12345678901234567890123456789012345678901234567890123456789012345678901234567890",
                                                             "57edf4a22be3c955ac49da2e2107b67a"},
}

for i, kv in ipairs(MD5_KAT) do
  s:test(string.format("md5 KAT #%d", i), function()
    T.eq(crypto.md5(kv[1]), kv[2])
  end)
end

s:test("md5 returns 32-char lowercase hex for arbitrary input", function()
  local h = crypto.md5(string.rep("x", 4096))
  T.eq(#h, 32)
  T.truthy(h:match("^[0-9a-f]+$"), "hex digits only")
end)

s:test("md5_binary returns a raw 16-byte string equal to hex(md5)", function()
  local raw = crypto.md5_binary("abc")
  T.eq(#raw, 16)
  T.eq(fan.data2hex(raw):lower(), "900150983cd24fb0d6963f7d28e17f72")
end)

-- ---- AES-GCM -------------------------------------------------------------

local gcm = crypto.gcm
T.not_nil(gcm, "fan.crypto.gcm must exist")

local function rep_byte(b, n)
  return string.rep(string.char(b), n)
end

s:test("AES-128-GCM: encrypt/decrypt round-trip (no AAD)", function()
  local key   = rep_byte(0x11, 16)
  local nonce = rep_byte(0x22, 12)
  local pt    = "hello, AES-GCM!"
  local ct, tag = gcm.encrypt(key, nonce, pt)
  T.is_type(ct,  "string")
  T.is_type(tag, "string")
  T.eq(#tag, 16)
  T.eq(#ct,  #pt)                         -- GCM has no padding

  local back = gcm.decrypt(key, nonce, ct, tag)
  T.eq(back, pt)
end)

s:test("AES-256-GCM: encrypt/decrypt round-trip with AAD", function()
  local key   = rep_byte(0x33, 32)
  local nonce = rep_byte(0x44, 12)
  local pt    = "payload"
  local aad   = "context-info"
  local ct, tag = gcm.encrypt(key, nonce, pt, aad)
  T.is_type(ct,  "string")
  T.eq(#tag, 16)

  T.eq(gcm.decrypt(key, nonce, ct, tag, aad), pt)

  -- Wrong AAD => tag mismatch => nil (not an error)
  T.is_nil(gcm.decrypt(key, nonce, ct, tag, "different"))

  -- Missing AAD => tag mismatch
  T.is_nil(gcm.decrypt(key, nonce, ct, tag))
end)

s:test("AES-GCM: tampering ciphertext -> tag mismatch -> nil", function()
  local key   = rep_byte(0x55, 16)
  local nonce = rep_byte(0x66, 12)
  local ct, tag = gcm.encrypt(key, nonce, "twelve bytes")
  -- Flip the first ciphertext byte
  local bad_ct = string.char(string.byte(ct, 1) ~ 0xff) .. ct:sub(2)
  T.is_nil(gcm.decrypt(key, nonce, bad_ct, tag))
end)

s:test("AES-GCM: tampering tag -> nil", function()
  local key   = rep_byte(0x77, 16)
  local nonce = rep_byte(0x88, 12)
  local ct, tag = gcm.encrypt(key, nonce, "some plaintext")
  local bad_tag = string.char(string.byte(tag, 1) ~ 0x01) .. tag:sub(2)
  T.is_nil(gcm.decrypt(key, nonce, ct, bad_tag))
end)

s:test("AES-GCM: empty plaintext round-trip yields empty ct + 16-byte tag", function()
  local key   = rep_byte(0x99, 16)
  local nonce = rep_byte(0xaa, 12)
  local ct, tag = gcm.encrypt(key, nonce, "")
  T.eq(ct, "")
  T.eq(#tag, 16)
  T.eq(gcm.decrypt(key, nonce, ct, tag), "")
end)

s:test("AES-GCM: invalid key length is a hard error", function()
  local ok, err = pcall(gcm.encrypt, rep_byte(0, 24), rep_byte(0, 12), "x")
  T.is_type(ok, "boolean")
  T.truthy(not ok, "24-byte key must be rejected")
  T.truthy(tostring(err):find("key must be"))
end)

s:test("AES-GCM: invalid nonce length is a hard error", function()
  local ok, err = pcall(gcm.encrypt, rep_byte(0, 16), rep_byte(0, 8), "x")
  T.truthy(not ok)
  T.truthy(tostring(err):find("nonce must be"))
end)

s:test("AES-GCM: invalid tag length on decrypt is a hard error", function()
  local ok, err = pcall(gcm.decrypt, rep_byte(0, 16), rep_byte(0, 12),
                        "", rep_byte(0, 8))
  T.truthy(not ok)
  T.truthy(tostring(err):find("tag must be"))
end)

s:test("AES-256-GCM NIST-style vector (empty pt, no AAD)", function()
  -- Reference: NIST GCM test vectors, Key=32B zeros, IV=12B zeros, PT="".
  -- Expected authentication tag from PyCryptodome/OpenSSL reference impls:
  --   530f8afbc74536b9a963b4f1c4cb738b
  local key   = rep_byte(0, 32)
  local nonce = rep_byte(0, 12)
  local ct, tag = gcm.encrypt(key, nonce, "")
  T.eq(ct, "")
  T.eq(fan.data2hex(tag):lower(), "530f8afbc74536b9a963b4f1c4cb738b")
end)

os.exit(T.run(s))
