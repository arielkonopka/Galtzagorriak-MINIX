# primes.be: a pure-computation example (spec R7c.6, issue #385).
# Run: berry /usr/lib/berry/primes.bec [N]  (default 100)
# Prints the primes below N and how many there are.
var n = size(_argv) > 1 ? int(_argv[1]) : 100
var sieve = []
for i: 0 .. n - 1  sieve.push(true) end
var found = []
for i: 2 .. n - 1
  if sieve[i]
    found.push(i)
    var j = i * i
    while j < n  sieve[j] = false  j += i end
  end
end
print(found.concat(" "))
print(format("%d primes below %d", size(found), n))
