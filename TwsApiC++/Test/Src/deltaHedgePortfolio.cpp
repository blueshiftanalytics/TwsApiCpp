#include "TwsApiL0.h"
#include "TwsApiDefs.h"
#include <cstdlib>
#include <iostream>
#include <vector>
#include <cmath>
#include <map>
	
using namespace TwsApi;
using namespace std;

bool error_for_request	           = false;
bool continue_request              = true;
unsigned int num_finished_requests = 0;
int order_id                       = 0;

//---------------------------------------------------------------------------------------------------------------------------------------
//
//---------------------------------------------------------------------------------------------------------------------------------------
struct Portfolio
{
    Portfolio(IBString acct) : account_str(acct) {}
    IBString account_str;
    vector <Contract> options;               // option contracts used to request data
    vector <int> quantities;                 // quantities that line up with option contracts
    map <const char*, int> current_stocks;   // <symbol, current_quantity>
    map <const char*, double> option_deltas; // <symbol, option_delta*quantity>
    map <const char*, int> stock_targets;    // target quantity of stocks for each symbol
};

//---------------------------------------------------------------------------------------------------------------------------------------
//                                                        MyEWrapper
//---------------------------------------------------------------------------------------------------------------------------------------
class MyEWrapper: public EWrapperL0, public Portfolio
{
	public:

		MyEWrapper( bool CalledFromThread = true , IBString account_str = "") : EWrapperL0( CalledFromThread ), Portfolio( account_str ) {}

		virtual void position( const IBString& account, const Contract& contract, int position, double avgCost)
		{
			if (contract.secType == "OPT" and account == account_str){
				options.push_back(contract); // will be used to get current delta
                quantities.push_back(position); // will be multiplied by delta to get overall position delta
            
            } else if (contract.secType == "STK" and account == account_str){
                current_stocks[contract.symbol] += position; // position is positive (long) or negative (short) quantity
            }
		}

		virtual void positionEnd()
		{
            // triggers when entire portfolio has been processed
			continue_request = false;
		}

        virtual void tickOptionComputation ( TickerId tickerId, TickType tickType, double impliedVol, double delta, double optPrice, 
                                             double pvDividend, double gamma, double vega, double theta, double undPrice)
        {
            // basing delta off the last traded option price
            if (tickType == 4){
                const char* symbol    = options[tickerId].symbol;
                option_deltas[symbol] = delta * quantities[tickerId];
                stock_targets[symbol] = round(-100*delta);
                num_finished_requests++;
            }
        }

        virtual void nextValidId( OrderId orderId )
        {
            order_id = orderId;
        }

        virtual void error( const int id, const int errorCode, const IBString errorString )
        {
            fprintf( stderr, "Error for id=%d: %d = %s\n", id, errorCode, (const char*)errorString );
            error_for_request = (id > 0);
            // id == -1 are 'system' messages, not for user requests
            // as a test, set year to 2010 in the reqHistoricalData
        }
};


//---------------------------------------------------------------------------------------------------------------------------------------
//																 MAIN
//---------------------------------------------------------------------------------------------------------------------------------------
int main( int argc, const char* argv[] )
{
    //---------------- COMMAND LINE PARSING -----------------//
    printf("Command line arguments: 1-Account_string | 2-coninuous (0 for no, 1 for yes)");
    printf(" | 3-max number of shares (absolute value), | 4-min share change\n");
    if (argc != 5){
        printf("ERROR: Incorrect command line arguments -- se above\n");
        return -1;
    }

    //-------------- DELTA HEDGING PARAMETERS ---------------//
    bool continuous_hedge = (bool) atoi(argv[2]);
    int max_num_shares    = atoi(argv[3]); // short or long
    int min_share_change  = atoi(argv[4]); // max number of shares to sell or buy in a single call

    //------------------- CONNECT TO IB ---------------------//
    MyEWrapper	MW( true , argv[1] );
    EClientL0*	EC = EClientL0::New( &MW );
    unsigned int i = 0;
    while ( i++, !EC->eConnect( "", 4002, 100 ) and i < 100 );

    //------------- MISC. VARIABLE DECLARATIONS -------------//
    map <const char*, int>::iterator it;

    Contract       C;
    C.secType      = "STK";
    C.currency     = "USD";
    C.exchange     = "SMART";

    Order          O;
    O.orderType    = "MKT";
    O.account      = argv[1];
    O.algoStrategy = "Adaptive";
    O.algoParams.reset(new TagValueList());
    TagValueSPtr tag1(new TagValue("adaptivePriority", "Urgent"));
    O.algoParams->push_back(tag1);

    int quantity;
    time_t t1, t2;
    vector <int> order_id_vec;

    //---------------------- MAIN LOOP ----------------------//
    do {

        order_id_vec.clear();
        MW.options.clear();
        MW.option_deltas.clear();    // make sure all data structures are clear at beginning of every loop
        MW.quantities.clear();
        MW.stock_targets.clear();
        MW.current_stocks.clear();
        num_finished_requests = 0;
        continue_request = true;

    //----------------- GET NECESSARY DATA ------------------//
        EC->reqPositions();
        while ( continue_request );
        for (i = 0; i < MW.options.size(); i++){
            EC->reqMktData(i, MW.options[i], "", false);
        }

        while ( num_finished_requests < MW.options.size() );

    //--------------- PLACE ALL HEDGE ORDERS ----------------//

        for (it = MW.stock_targets.begin(); it != MW.stock_targets.end(); it++){
            C.symbol = it->first;

            // check the target number of shares against the max_num_shares
            if      (it->second > max_num_shares)  it->second = max_num_shares;
            else if (it->second < -max_num_shares) it->second = -max_num_shares;

            // set quantity and adjust for min_share_change variable
            // this is the part that determines whether to place an order or skip
            // quantity = target - current
            quantity = it->second - MW.current_stocks[C.symbol];
            if (abs(quantity) < min_share_change) continue;

            // determine whether we're buying or selling
            if      (quantity > 0) O.action = "BUY";  // positive quantity means buy
            else if (quantity < 0) O.action = "SELL"; // negative quantity means sell
            O.totalQuantity                 = abs(quantity);

            // before placing order for real, we need to check the margin requirement
            EC->placeOrder(order_id , C , O);
            order_id_vec.push_back(order_id);
            order_id++;
        }
        
        // wait for 30 seconds for all orders to process, then cancel pending orders from previous run through
        time(&t1);
        while (time(&t2), t2-t1 < 30);
        for (i = 0; i < order_id_vec.size(); i++){
            EC->cancelOrder(order_id_vec[i]);
        }

    } while ( continuous_hedge and EC->IsConnected() );

    EC->eDisconnect();
    delete EC;
    return error_for_request;
}
