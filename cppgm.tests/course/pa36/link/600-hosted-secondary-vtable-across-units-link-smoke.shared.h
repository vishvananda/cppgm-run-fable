#pragma once

struct ISink {
  virtual void put(int value) = 0;
  virtual ~ISink() {}
};

struct ILine {
  virtual void line(long number) = 0;
  virtual ~ILine() {}
};

class Collector : public ISink, public ILine {
 public:
  Collector() : total_(0) {}
  void put(int value);
  void line(long number) { total_ += 100 * number; }
  long total() const { return total_; }

 private:
  long total_;
};

long drive(ISink& sink, ILine& lines);
