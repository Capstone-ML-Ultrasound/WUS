#include <string>
#include <vector>


class Utils {
    public:
        Utils();
        ~Utils();

        bool writeCSV(std::vector<unsigned char>& samples);
        bool writeBurstCSV(const std::vector<std::vector<unsigned char>>& burstData);


    private:
    
};


