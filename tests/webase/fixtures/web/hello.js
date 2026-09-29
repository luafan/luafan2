// tiny fixture used by webase integration tests to exercise gzip / ETag /
// mime-type / 304 branches. Kept small (~200 B) but highly compressible
// so gzip encoding is guaranteed smaller than the raw body.
(function () {
  var msg = "hello, hello, hello, webase, hello, hello, hello, webase!";
  console.log(msg);
  console.log(msg);
  console.log(msg);
})();
